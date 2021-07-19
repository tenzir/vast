#!/bin/bash

# Extracts compiler and linker flags from a ninja file and
# converts them into a JSON list of strings.

INFILE="$1"
OUTFILE="$2"

NINJARULES="$3"
CC_PATTERN="$4"
LD_PATTERN="$5"

# From the rule that build fuzzer.cpp -> fuzzer.cpp.o, we extract
# cflags, includes, defines, etc.
CC_RULE_DEFINITION=$(grep -A 7 -e "^${CC_PATTERN}" ${NINJARULES})
DEFINES=$(echo "${CC_RULE_DEFINITION}" | grep -e "DEFINES = ")
CFLAGS=$(echo "${CC_RULE_DEFINITION}" | grep -e "FLAGS = ")
INCLUDES=$(echo "${CC_RULE_DEFINITION}" | grep -e "INCLUDES = ")

# From the rule that links fuzzer.cpp.o -> fuzzer, we extract
# the linker flags.
LD_RULE_DEFINITION=$(grep -A 7 -e "^${LD_PATTERN}" ${NINJARULES})
LDFLAGS=$(echo "${LD_RULE_DEFINITION}" | grep -e "FLAGS = ")
LINK_LIBRARIES=$(echo "${LD_RULE_DEFINITION}" | grep -e "LINK_LIBRARIES = ")

# Convert the  "FOO = A B C" lines to a json list
JLIST=$(echo "${DEFINES}" | sed -r "s/DEFINES = (.*)$/\1/" | sed 's/ /\n/g' | awk 'NF' | sed -r 's/^(.*)$/  - "\1"/')
JLIST=${JLIST}"\n"$(echo "${CFLAGS}" | sed -r "s/FLAGS = (.*)$/\1/" | sed 's/ /\n/g'| awk 'NF' | sed -r 's/^(.*)$/  - "\1"/')
JLIST=${JLIST}"\n"$(echo "${INCLUDES}" | sed -r "s/INCLUDES = (.*)$/\1/" | sed 's/ /\n/g'|  awk 'NF' | sed -r 's/^(.*)$/  - "\1"/')
JLIST=${JLIST}"\n"$(echo "${LDFLAGS}" | sed -r "s/FLAGS = (.*)$/\1/" | sed 's/ /\n/g'| awk 'NF'  | sed -r 's/^(.*)$/  - "\1"/')

# Don't bother splitting the linker command line and just append it as a single string.
JLIST=${JLIST}"\n"$(echo "${LINK_LIBRARIES}" | sed -r "s/LINK_LIBRARIES = (.*)$/\1/" | sed -r 's/^(.*)$/  - "\1"/')


awk -v r="${JLIST}" '{gsub(/@@JSON_BUILD_FLAGS@@/,r)}1' ${INFILE} >${OUTFILE}
