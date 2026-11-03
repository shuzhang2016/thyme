// SEC-08: atomic HMAC key installation, archival, and cross-rotation checks.
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "thyme/check.h"
#include "thyme/record/package_integrity.h"

namespace {

namespace fs = std::filesystem;

void WriteText(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  CHECK(out.good());
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  CHECK(out.good());
}

std::string ReadBinary(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  CHECK(in.good());
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

bool GeneratedKey(const std::string& key) {
  return key.size() == 65 && key.back() == '\n' &&
         std::all_of(key.begin(), key.end() - 1, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

void ExpectMode600(const std::string& path) {
  struct stat st {};
  CHECK(::stat(path.c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
}

pid_t StartRotate(const char* script, const std::string& key,
                  const std::string& archive_dir,
                  const char* owner = nullptr) {
  const pid_t pid = ::fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    if (owner == nullptr) {
      ::execl(script, script, "--key", key.c_str(), "--archive-dir",
              archive_dir.c_str(), static_cast<char*>(nullptr));
    } else {
      ::execl(script, script, "--key", key.c_str(), "--archive-dir",
              archive_dir.c_str(), "--owner", owner,
              static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  return pid;
}

int WaitForRotation(pid_t pid) {
  int status = 0;
  CHECK(::waitpid(pid, &status, 0) == pid);
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return 128 + WTERMSIG(status);
}

int Rotate(const char* script, const std::string& key,
           const std::string& archive_dir, const char* owner = nullptr) {
  return WaitForRotation(StartRotate(script, key, archive_dir, owner));
}

void MakePackage(const std::string& dir, const std::string& key_path) {
  fs::create_directories(dir);
  WriteText(dir + "/meta.tbin", "metadata\n");
  WriteText(dir + "/wal.log", "seal\n");
  thyme::integrity::PackageInfo info;
  info.seal_time_ns = 123456789;
  info.session_id = 7;
  info.trigger_id = 11;
  std::string err;
  CHECK(thyme::integrity::WritePackageManifest(dir, info, key_path, &err));
}

void ExpectVerification(const std::string& dir, const std::string& key_path,
                        bool expected) {
  const thyme::integrity::VerifyResult result =
      thyme::integrity::VerifyPackageManifest(dir, key_path);
  CHECK(result.ok() == expected);
  CHECK(result.signature_checked);
  CHECK(result.signature_ok == expected);
}

std::vector<std::string> ArchivedKeys(const std::string& archive_dir) {
  std::vector<std::string> keys;
  for (const fs::directory_entry& entry : fs::directory_iterator(archive_dir)) {
    CHECK(entry.is_regular_file());
    keys.push_back(ReadBinary(entry.path().string()));
    ExpectMode600(entry.path().string());
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

void ExpectNoTemporaryKeys(const std::string& key_dir) {
  for (const fs::directory_entry& entry : fs::directory_iterator(key_dir)) {
    CHECK(entry.path().filename().string().find(".new.") == std::string::npos);
    CHECK(entry.path().filename().string().find(".old.") == std::string::npos);
  }
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  const char* script = argv[1];
  char temp[] = "/tmp/thyme_sec08_XXXXXX";
  CHECK(::mkdtemp(temp) != nullptr);
  const std::string root = temp;

  const std::string initial_key_dir = root + "/initial/active";
  const std::string initial_archive_dir = root + "/initial/archive";
  fs::create_directories(initial_key_dir);
  fs::create_directories(initial_archive_dir);
  const std::string initial_active = initial_key_dir + "/evidence.key";
  CHECK(!fs::exists(initial_active));
  CHECK(Rotate(script, initial_active, initial_archive_dir) == 0);
  CHECK(GeneratedKey(ReadBinary(initial_active)));
  ExpectMode600(initial_active);
  CHECK(ArchivedKeys(initial_archive_dir).empty());
  ExpectNoTemporaryKeys(initial_key_dir);

  const std::string key_dir = root + "/active";
  const std::string archive_dir = root + "/archive";
  fs::create_directories(key_dir);
  fs::create_directories(archive_dir);
  const std::string active = key_dir + "/evidence.key";
  const std::string old_key = "old-key-material\n";
  WriteText(active, old_key);
  CHECK(::chmod(active.c_str(), 0644) == 0);

  MakePackage(root + "/old-package", active);
  ExpectVerification(root + "/old-package", active, true);

  CHECK(Rotate(script, active, archive_dir) == 0);
  const std::string first_key = ReadBinary(active);
  CHECK(GeneratedKey(first_key));
  CHECK(first_key != old_key);
  ExpectMode600(active);
  CHECK(ArchivedKeys(archive_dir) == std::vector<std::string>{old_key});
  ExpectNoTemporaryKeys(key_dir);

  MakePackage(root + "/first-package", active);
  ExpectVerification(root + "/old-package", active, false);
  ExpectVerification(root + "/first-package", active, true);
  WriteText(root + "/wrong.key", "wrong-key-material\n");
  ExpectVerification(root + "/old-package", root + "/wrong.key", false);
  ExpectVerification(root + "/first-package", root + "/wrong.key", false);

  CHECK(Rotate(script, active, archive_dir) == 0);
  const std::string second_key = ReadBinary(active);
  CHECK(GeneratedKey(second_key));
  CHECK(second_key != first_key);
  ExpectMode600(active);
  std::vector<std::string> expected_archive{first_key, old_key};
  std::sort(expected_archive.begin(), expected_archive.end());
  CHECK(ArchivedKeys(archive_dir) == expected_archive);
  ExpectNoTemporaryKeys(key_dir);

  MakePackage(root + "/second-package", active);
  ExpectVerification(root + "/second-package", active, true);
  ExpectVerification(root + "/second-package", root + "/wrong.key", false);

  // Verify every generation with its retained key, independently of filename
  // timestamps or archive ordering.
  for (const fs::directory_entry& entry : fs::directory_iterator(archive_dir)) {
    const std::string archived = entry.path().string();
    const std::string contents = ReadBinary(archived);
    if (contents == old_key) {
      ExpectVerification(root + "/old-package", archived, true);
    } else if (contents == first_key) {
      ExpectVerification(root + "/first-package", archived, true);
    } else {
      CHECK(false);
    }
  }

  // Neither a pre-flight failure nor a failure after key generation can replace
  // or truncate the active key. The EXIT trap removes the temporary key.
  const std::string not_a_directory = root + "/not-a-directory";
  WriteText(not_a_directory, "keep\n");
  CHECK(Rotate(script, active, not_a_directory) != 0);
  CHECK(ReadBinary(active) == second_key);
  CHECK(ReadBinary(not_a_directory) == "keep\n");
  CHECK(Rotate(script, active, archive_dir, "thyme-invalid-owner") != 0);
  CHECK(ReadBinary(active) == second_key);
  CHECK(ArchivedKeys(archive_dir) == expected_archive);
  ExpectNoTemporaryKeys(key_dir);

  // Start all rotations before waiting for any one of them. Every successful
  // generation must occur exactly once across the active key and the archive.
  constexpr uint32_t kConcurrentRotations = 4;
  std::vector<pid_t> concurrent_rotations;
  for (uint32_t i = 0; i < kConcurrentRotations; ++i) {
    concurrent_rotations.push_back(StartRotate(script, active, archive_dir));
  }
  for (const pid_t pid : concurrent_rotations) {
    CHECK(WaitForRotation(pid) == 0);
  }

  const std::string final_key = ReadBinary(active);
  CHECK(GeneratedKey(final_key));
  CHECK(final_key != second_key);
  ExpectMode600(active);
  ExpectMode600(active + ".lock");
  expected_archive.push_back(second_key);
  std::sort(expected_archive.begin(), expected_archive.end());
  const std::vector<std::string> concurrent_archive = ArchivedKeys(archive_dir);
  CHECK(concurrent_archive.size() ==
        expected_archive.size() + kConcurrentRotations - 1);
  CHECK(std::adjacent_find(concurrent_archive.begin(),
                           concurrent_archive.end()) == concurrent_archive.end());
  CHECK(std::includes(concurrent_archive.begin(), concurrent_archive.end(),
                      expected_archive.begin(), expected_archive.end()));
  CHECK(std::find(concurrent_archive.begin(), concurrent_archive.end(),
                  final_key) == concurrent_archive.end());
  ExpectNoTemporaryKeys(key_dir);

  uint32_t concurrent_packages = 0;
  for (const fs::directory_entry& entry : fs::directory_iterator(archive_dir)) {
    const std::string archived = entry.path().string();
    const std::string contents = ReadBinary(archived);
    if (contents == old_key) {
      ExpectVerification(root + "/old-package", archived, true);
    } else if (contents == first_key) {
      ExpectVerification(root + "/first-package", archived, true);
    } else if (contents == second_key) {
      ExpectVerification(root + "/second-package", archived, true);
    } else {
      const std::string package =
          root + "/concurrent-package-" + std::to_string(concurrent_packages);
      MakePackage(package, archived);
      ExpectVerification(package, archived, true);
      ExpectVerification(package, root + "/wrong.key", false);
      ++concurrent_packages;
    }
  }
  const std::string final_package =
      root + "/concurrent-package-" + std::to_string(concurrent_packages);
  MakePackage(final_package, active);
  ExpectVerification(final_package, active, true);
  ExpectVerification(final_package, root + "/wrong.key", false);
  ++concurrent_packages;
  CHECK(concurrent_packages == kConcurrentRotations);

  fs::remove_all(root);
  std::printf("SEC-08 signing-key rotation checks passed\n");
  return 0;
}
