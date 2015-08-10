//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2010/10/27
// Author: Dan Adkins
//
// Copyright 2010 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// \file time.cc
// \brief time related functions
//
//----------------------------------------------------------------------------

#include <sys/time.h>
#include "/usr/include/time.h"  //subrata
#include <sys/resource.h>
#include "time.h"
#include <stdio.h>
#include <unistd.h>

namespace KFS {

int64_t
microseconds(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, 0) < 0)
        return -1;

    return (int64_t)tv.tv_sec*1000*1000 + tv.tv_usec;
}
void getTimeStrinMiliSecCostly(char* timeStr)
{
  struct timeval tv;
 struct tm* ptm;
 long milliseconds;
 char time_string[40];

   /* Obtain the time of day, and convert it to a tm struct. */
 gettimeofday (&tv, NULL);
 ptm = localtime (&tv.tv_sec);
 /* Format the date and time, down to a single second. */
 strftime (time_string, sizeof (time_string), "%Y-%m-%d %H:%M:%S", ptm);
 /* Compute milliseconds from microseconds. */
 milliseconds = tv.tv_usec / 1000;
 /* Print the formatted time, in seconds, followed by a decimal point
   and the milliseconds. */
  sprintf (timeStr, "%s.%03ld\n", time_string, milliseconds);
}


int64_t
cputime(int64_t *user, int64_t *sys)
{
    struct rusage ru;

    if (getrusage(RUSAGE_SELF, &ru) < 0)
        return -1;

    *user = (int64_t)ru.ru_utime.tv_sec*1000*1000 + ru.ru_utime.tv_usec;
    *sys  = (int64_t)ru.ru_stime.tv_sec*1000*1000 + ru.ru_stime.tv_usec;

    return *user + *sys;
}

} // namespace KFS
