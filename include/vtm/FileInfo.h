//===-==-- vtm/FileInfo.h - The FileInfo manage the output files ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file define the FileInfo pass, which manage the various output files
// of the backend.
//
//===----------------------------------------------------------------------===//

#ifndef VTM_FILE_INFO_H
#define VTM_FILE_INFO_H
#include "llvm/ADT/SmallVector.h"

#include <string>

namespace llvm {
struct VTargetMachine;
class tool_output_file;

class FileInfo{
  // DO NOT IMPLEMENT
  FileInfo(const FileInfo&);
  // DO NOT IMPLEMENT
  const FileInfo &operator=(const FileInfo&);

  // Write all contents to stdout, for debug use.
  bool WriteAllToStdOut;

  // The directory for output files. 
  std::string OutFilesDir;

  // The The name of the hardware sub system. 
  std::string HWSubSysName;

  SmallVector<tool_output_file*, 2> OpenedFiles;

  // Configuration accessor.
  std::string getOutFilePath(const std::string &Name,
                             const std::string &Suffix) const;


  bool writeAllToStdOut() const {
    return HWSubSysName.empty() || WriteAllToStdOut;
  }

  void setOutFilesDir(const std::string &Val);

  const std::string &getOutFilesDir() const {
    return OutFilesDir;
  }

  friend struct VTargetMachine;
public:
  FileInfo() : WriteAllToStdOut(false) {}
  ~FileInfo();

  const std::string &getHWSubSysName() const {
    return HWSubSysName;
  }

  tool_output_file *getOutFile(const std::string &Suffix, unsigned Flags = 0);
};

FileInfo &vtmfiles();

}

#endif
