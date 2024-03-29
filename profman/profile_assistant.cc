/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "profile_assistant.h"

#include "base/os.h"
#include "base/unix_file/fd_file.h"
#include "profman/profman_result.h"

namespace art {

// Minimum number of new methods/classes that profiles
// must contain to enable recompilation.
static constexpr const uint32_t kMinNewMethodsForCompilation = 100;
static constexpr const uint32_t kMinNewClassesForCompilation = 50;

ProfmanResult::ProcessingResult ProfileAssistant::ProcessProfilesInternal(
    const std::vector<ScopedFlock>& profile_files,
    const ScopedFlock& reference_profile_file,
    const ProfileCompilationInfo::ProfileLoadFilterFn& filter_fn,
    const Options& options) {
  ProfileCompilationInfo info(options.IsBootImageMerge());

  // Load the reference profile.
  if (!info.Load(reference_profile_file->Fd(), /*merge_classes=*/ true, filter_fn)) {
    LOG(WARNING) << "Could not load reference profile file";
    return ProfmanResult::kErrorBadProfiles;
  }

  if (options.IsBootImageMerge() && !info.IsForBootImage()) {
    LOG(WARNING) << "Requested merge for boot image profile but the reference profile is regular.";
    return ProfmanResult::kErrorBadProfiles;
  }

  // Store the current state of the reference profile before merging with the current profiles.
  uint32_t number_of_methods = info.GetNumberOfMethods();
  uint32_t number_of_classes = info.GetNumberOfResolvedClasses();

  // Merge all current profiles.
  for (size_t i = 0; i < profile_files.size(); i++) {
    ProfileCompilationInfo cur_info(options.IsBootImageMerge());
    if (!cur_info.Load(profile_files[i]->Fd(), /*merge_classes=*/ true, filter_fn)) {
      LOG(WARNING) << "Could not load profile file at index " << i;
      if (options.IsForceMerge() || options.IsForceMergeAndAnalyze()) {
        // If we have to merge forcefully, ignore load failures.
        // This is useful for boot image profiles to ignore stale profiles which are
        // cleared lazily.
        continue;
      }
      // TODO: Do we really need to use a different error code for version mismatch?
      ProfileCompilationInfo wrong_info(!options.IsBootImageMerge());
      if (wrong_info.Load(profile_files[i]->Fd(), /*merge_classes=*/ true, filter_fn)) {
        return ProfmanResult::kErrorDifferentVersions;
      }
      return ProfmanResult::kErrorBadProfiles;
    }

    if (!info.MergeWith(cur_info)) {
      LOG(WARNING) << "Could not merge profile file at index " << i;
      return ProfmanResult::kErrorBadProfiles;
    }
  }

  // If we perform a forced merge do not analyze the difference between profiles.
  if (!options.IsForceMerge()) {
    if (info.IsEmpty()) {
      return ProfmanResult::kSkipCompilationEmptyProfiles;
    }

    if (options.IsForceMergeAndAnalyze()) {
      // When we force merge and analyze, we want to always recompile unless there is absolutely no
      // difference between before and after the merge (i.e., the classes and methods in the
      // reference profile were already a superset of those in all current profiles before the
      // merge.)
      if (info.GetNumberOfMethods() == number_of_methods &&
          info.GetNumberOfResolvedClasses() == number_of_classes) {
        return ProfmanResult::kSkipCompilationSmallDelta;
      }
    } else {
      uint32_t min_change_in_methods_for_compilation = std::max(
          (options.GetMinNewMethodsPercentChangeForCompilation() * number_of_methods) / 100,
          kMinNewMethodsForCompilation);
      uint32_t min_change_in_classes_for_compilation = std::max(
          (options.GetMinNewClassesPercentChangeForCompilation() * number_of_classes) / 100,
          kMinNewClassesForCompilation);
      // Check if there is enough new information added by the current profiles.
      if (((info.GetNumberOfMethods() - number_of_methods) <
           min_change_in_methods_for_compilation) &&
          ((info.GetNumberOfResolvedClasses() - number_of_classes) <
           min_change_in_classes_for_compilation)) {
        return ProfmanResult::kSkipCompilationSmallDelta;
      }
    }
  }

  // We were successful in merging all profile information. Update the reference profile.
  if (!reference_profile_file->ClearContent()) {
    PLOG(WARNING) << "Could not clear reference profile file";
    return ProfmanResult::kErrorIO;
  }
  if (!info.Save(reference_profile_file->Fd())) {
    LOG(WARNING) << "Could not save reference profile file";
    return ProfmanResult::kErrorIO;
  }

  return options.IsForceMerge() ? ProfmanResult::kSuccess : ProfmanResult::kCompile;
}

class ScopedFlockList {
 public:
  explicit ScopedFlockList(size_t size) : flocks_(size) {}

  // Will block until all the locks are acquired.
  bool Init(const std::vector<std::string>& filenames, /* out */ std::string* error) {
    for (size_t i = 0; i < filenames.size(); i++) {
      flocks_[i] = LockedFile::Open(filenames[i].c_str(), O_RDWR, /* block= */ true, error);
      if (flocks_[i].get() == nullptr) {
        *error += " (index=" + std::to_string(i) + ")";
        return false;
      }
    }
    return true;
  }

  // Will block until all the locks are acquired.
  bool Init(const std::vector<int>& fds, /* out */ std::string* error) {
    for (size_t i = 0; i < fds.size(); i++) {
      DCHECK_GE(fds[i], 0);
      flocks_[i] = LockedFile::DupOf(fds[i], "profile-file",
                                     /* read_only_mode= */ true, error);
      if (flocks_[i].get() == nullptr) {
        *error += " (index=" + std::to_string(i) + ")";
        return false;
      }
    }
    return true;
  }

  const std::vector<ScopedFlock>& Get() const { return flocks_; }

 private:
  std::vector<ScopedFlock> flocks_;
};

ProfmanResult::ProcessingResult ProfileAssistant::ProcessProfiles(
        const std::vector<int>& profile_files_fd,
        int reference_profile_file_fd,
        const ProfileCompilationInfo::ProfileLoadFilterFn& filter_fn,
        const Options& options) {
  DCHECK_GE(reference_profile_file_fd, 0);

  std::string error;
  ScopedFlockList profile_files(profile_files_fd.size());
  if (!profile_files.Init(profile_files_fd, &error)) {
    LOG(WARNING) << "Could not lock profile files: " << error;
    return ProfmanResult::kErrorCannotLock;
  }

  // The reference_profile_file is opened in read/write mode because it's
  // cleared after processing.
  ScopedFlock reference_profile_file = LockedFile::DupOf(reference_profile_file_fd,
                                                         "reference-profile",
                                                         /* read_only_mode= */ false,
                                                         &error);
  if (reference_profile_file.get() == nullptr) {
    LOG(WARNING) << "Could not lock reference profiled files: " << error;
    return ProfmanResult::kErrorCannotLock;
  }

  return ProcessProfilesInternal(profile_files.Get(),
                                 reference_profile_file,
                                 filter_fn,
                                 options);
}

ProfmanResult::ProcessingResult ProfileAssistant::ProcessProfiles(
        const std::vector<std::string>& profile_files,
        const std::string& reference_profile_file,
        const ProfileCompilationInfo::ProfileLoadFilterFn& filter_fn,
        const Options& options) {
  std::string error;

  ScopedFlockList profile_files_list(profile_files.size());
  if (!profile_files_list.Init(profile_files, &error)) {
    LOG(WARNING) << "Could not lock profile files: " << error;
    return ProfmanResult::kErrorCannotLock;
  }

  ScopedFlock locked_reference_profile_file = LockedFile::Open(
      reference_profile_file.c_str(), O_RDWR, /* block= */ true, &error);
  if (locked_reference_profile_file.get() == nullptr) {
    LOG(WARNING) << "Could not lock reference profile files: " << error;
    return ProfmanResult::kErrorCannotLock;
  }

  return ProcessProfilesInternal(profile_files_list.Get(),
                                 locked_reference_profile_file,
                                 filter_fn,
                                 options);
}

}  // namespace art
