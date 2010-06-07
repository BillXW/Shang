/*
* Copyright: 2008 by Nadav Rotem. all rights reserved.
* IMPORTANT: This software is supplied to you by Nadav Rotem in consideration
* of your agreement to the following terms, and your use, installation, 
* modification or redistribution of this software constitutes acceptance
* of these terms.  If you do not agree with these terms, please do not use, 
* install, modify or redistribute this software. You may not redistribute, 
* install copy or modify this software without written permission from 
* Nadav Rotem. 
*/

#ifndef LLVM_PARAMS_H
#define LLVM_PARAMS_H

#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Constants.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Module.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CFG.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Support/CommandLine.h"

#include <map>
#include <algorithm>
#include <sstream>


using std::map;
using std::string;
using std::stringstream;

using namespace llvm;

namespace xVerilog {

// define the integer parser

class ResourceConfig : public ImmutablePass {
  
  typedef std::map<string, unsigned int> ResTabTy;

  static ResTabTy ResTab;

public:
  static char ID;
  explicit ResourceConfig() : ImmutablePass(&ID) {};

  virtual void initializePass();
  /*
  * Load all of the values into a structure which will be used by the scheduler
  * to build the hardware description table.
  */
  static unsigned int getResConfig(std::string ResName) {
    ResTabTy::iterator at = ResTab.find(ResName);
    return at == ResTab.end()? 0 : at->second;
  }
  // FIXME: This could be done by vbe.
  static std::string  chrsubst(string str , int ch, int ch2) { //JAWAD
    char *s1   = new char [str.size()+1];  
    strcpy (s1, str.c_str());
    int count = 0; /* The count to return */
    char *wrk = strchr(s1, ch); /* Find first char in s1 */
    while (wrk) { /* While we have matches */
      *wrk = (char) ch2; /* Replace the character */
      count++, wrk++; /* Increment the count & pointer */
      wrk = strchr(wrk, ch); /* Search for next occurance */
    }
    //return count; /* Return the count */
    return  string(s1);
  }   
}; //class

} // namespace

#endif // h guard

