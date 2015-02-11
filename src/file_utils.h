#ifndef SRC_FILEUTILS_H_
#define SRC_FILEUTILS_H_

#include "src/data_container.h"

#include <wordexp.h>
#include <fstream>

namespace CthunAgent {
namespace FileUtils {

//
// Free functions
//

/// Perform a shell expansion of txt.
/// Return an empty string in case of failure.
std::string shellExpand(std::string txt);

/// Return true if the specified file exists.
bool fileExists(const std::string& file_path);

/// Remove a file (regular file, symlink, or empty dir) if exists.
/// Throw a file_error if the removal fails.
void removeFile(const std::string& file_path);

/// Write content to file in the specified mode.
/// Throw a file_error in case it fails to open the file to write.
void streamToFile(const std::string& text,
                  const std::string&  file_path,
                  std::ios_base::openmode mode);

/// Write content to file. If file exists, its previous content will
/// be deleted.
/// Throw a file_error in case it fails to open file to write.
void writeToFile(const std::string& text,
                 const std::string& file_path);

/// Create a directory.
/// Returns true on success, false on failure.
bool createDirectory(const std::string& dirname);

/// Read the content of a file and returns it as a string.
std::string readFileAsString(std::string path);

}  // namespace FileUtils
}  // namespace CthunAgent

#endif  // SRC_FILEUTILS_H_
