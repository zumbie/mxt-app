//------------------------------------------------------------------------------
/// \file   gr.c
/// \brief  T66 golden reference calibration
/// \author Nick Dyer
//------------------------------------------------------------------------------
// Copyright 2012 Atmel Corporation. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
//    2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY ATMEL ''AS IS'' AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT SHALL ATMEL OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
// OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//------------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "libmaxtouch/libmaxtouch.h"
#include "libmaxtouch/info_block.h"
#include "libmaxtouch/utilfuncs.h"
#include "libmaxtouch/log.h"

#include "gr.h"

#define GR_CTRL              0

#define GR_ENABLE            (1 << 0)
#define GR_RPTEN             (1 << 1)
#define GR_FCALCMD_PRIME     (1 << 2)
#define GR_FCALCMD_GENERATE  (1 << 3)
#define GR_FCALCMD_STORE     (GR_FCALCMD_PRIME | GR_FCALCMD_GENERATE)
#define GR_FCALCMD_MASK      GR_FCALCMD_STORE
#define GR_TESTONINIT        (1 << 4)
#define GR_TESTONCAL         (1 << 5)

#define GR_STATE_BADSTOREDATA     (1 << 0)
#define GR_STATE_IDLE             0
#define GR_STATE_PRIMED           (1 << 1)
#define GR_STATE_GENERATED        (1 << 2)
#define GR_STATE_FCALSTATE_MASK   (GR_STATE_PRIMED | GR_STATE_GENERATED)
#define GR_STATE_FCALSEQERR       (1 << 3)
#define GR_STATE_FCALSEQTO        (1 << 4)
#define GR_STATE_FCALSEQDONE      (1 << 5)
#define GR_STATE_FCALPASS         (1 << 6)
#define GR_STATE_FCALFAIL         (1 << 7)

#define GR_TIMEOUT            30

//******************************************************************************
/// \brief Handle status messages from the T6 command processor object
static void mxt_gr_print_t6_state(uint8_t state)
{
  printf("T6 status: %s%s%s%s%s%s\n",
         (state & 0x04) ? "COMSERR ":"",
         (state & 0x08) ? "CFGERR ":"",
         (state & 0x10) ? "CAL ":"",
         (state & 0x20) ? "SIGERR ":"",
         (state & 0x40) ? "OFL ":"",
         (state & 0x80) ? "RESET ":"");
}

//******************************************************************************
/// \brief Handle status messages from the T66 golden references object
static void mxt_gr_print_state(uint8_t state)
{
  printf("T66 state: %02X %s%s%s%s%s%s%s%s%s\n",
      state,
      (state & GR_STATE_FCALFAIL) ? "FCALFAIL " : "",
      (state & GR_STATE_FCALPASS) ? "FCALPASS " : "",
      (state & GR_STATE_FCALSEQDONE) ? "FCALSEQDONE " : "",
      (state & GR_STATE_FCALSEQTO) ? "FCALSEQTO " : "",
      (state & GR_STATE_FCALSEQERR) ? "FCALSEQERR " : "",
      ((state & GR_STATE_FCALSTATE_MASK) == GR_STATE_IDLE)     ? "Idle " : "",
      ((state & GR_STATE_FCALSTATE_MASK) == GR_STATE_GENERATED)?"Generated ":"",
      ((state & GR_STATE_FCALSTATE_MASK) == GR_STATE_PRIMED)   ? "Primed " : "",
      (state & GR_STATE_BADSTOREDATA) ? "BADSTOREDATA " : "");
}

//******************************************************************************
/// \brief Handle status messages from the T66 golden references object
static int mxt_gr_get_status(uint8_t *state, int timeout_seconds)
{
  uint16_t count, i, byte;
  time_t now;
  time_t start_time = time(NULL);
  uint8_t buf[10];
  size_t len;
  unsigned int object_type;

  while (true)
  {
    now = time(NULL);
    if ((now - start_time) > timeout_seconds)
    {
      printf("Timeout\n");
      return -1;
    }

    count = mxt_get_msg_count();

    if (count > 0)
    {
      for (i = 0; i < count; i++)
      {
        len = mxt_get_msg_bytes(buf, sizeof(buf));

        if (len > 0)
        {
          object_type = report_id_to_type(buf[0]);

          LOG(LOG_VERBOSE, "Received message from T%u", object_type);

          if (object_type == SPT_GOLDENREFERENCES_T66)
          {
            for (byte = 1; byte < len; byte++)
            {
              printf("%02X ", buf[byte]);
            }
            printf("\n");

            *state = buf[1];
            mxt_gr_print_state(*state);
            return 0;
          }
          else if (object_type == GEN_COMMANDPROCESSOR_T6)
          {
            mxt_gr_print_t6_state(buf[1]);
          }
        }
      }
    }

    sleep(1);
  }
}

//******************************************************************************
/// \brief Send command then check status
static int mxt_gr_run_command(uint16_t addr, uint8_t cmd,
                              uint8_t wanted_fcal_state, uint8_t wanted_statebit)
{
  int ret;
  uint8_t actual_state;

  cmd |= GR_ENABLE | GR_RPTEN;

  LOG(LOG_INFO, "Writing %u to ctrl register", cmd);
  ret = mxt_write_register(&cmd, addr + GR_CTRL, 1);
  if (ret < 0)
    return ret;

  ret = mxt_gr_get_status(&actual_state, GR_TIMEOUT);
  if (ret < 0)
    return ret;

  if (((actual_state & GR_STATE_FCALSTATE_MASK) == wanted_fcal_state)
      && (actual_state & wanted_statebit))
  {
    return 0;
  }
  else
  {
    printf("Failed to enter correct state\n");
    return -1;
  }
}

//******************************************************************************
/// \brief Store golden reference calibration
int mxt_store_golden_refs()
{
  uint16_t addr;
  int ret;

  ret = mxt_msg_reset();
  if (ret < 0)
    return ret;

  addr = get_object_address(SPT_GOLDENREFERENCES_T66, 0);
  if (addr == OBJECT_NOT_FOUND)
    return -1;

  printf("Priming\n");
  ret = mxt_gr_run_command(addr, GR_FCALCMD_PRIME,
                           GR_STATE_PRIMED, GR_STATE_PRIMED);
  if (ret < 0)
    return ret;

  printf("Generating\n");
  ret = mxt_gr_run_command(addr, GR_FCALCMD_GENERATE,
                           GR_STATE_GENERATED, GR_STATE_FCALPASS);
  if (ret < 0)
    return ret;

  printf("Storing\n");
  ret = mxt_gr_run_command(addr, GR_FCALCMD_STORE,
                          GR_STATE_IDLE, GR_STATE_FCALSEQDONE);
  if (ret < 0)
    return ret;

  printf("Done\n");
  return 0;
}
