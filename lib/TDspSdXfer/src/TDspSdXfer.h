// SPDX-License-Identifier: MIT
// (c) 2026 T-DSP project.
//
// TDspSdXfer — umbrella include for host<->device SD file transfer.
//
//   SdWriteReceiver   receives a file PUSHED from the host onto the SD card
//                     (the write-direction complement of '@READ'/streamFile).
//
// See README.md for the wire protocol and how to wire the receiver into an
// existing '@'-command serial ingest loop.

#pragma once

#include "SdWriteReceiver.h"
