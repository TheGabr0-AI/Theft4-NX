/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <memory>

#include <rex/ppc/function.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>
#include <rex/thread.h>

#include <mutex>  // blur-nx (2026-11-13, M-38)

// All of the exported functions:
#include <rex/kernel/xboxkrnl/rtl.h>

namespace rex::kernel::xboxkrnl {

class XboxkrnlModule : public system::KernelModule {
 public:
  XboxkrnlModule(Runtime* emulator, system::KernelState* kernel_state);
  virtual ~XboxkrnlModule();

  static void RegisterExportTable(rex::runtime::ExportResolver* export_resolver);

  bool SendPIXCommand(const char* cmd);

  void set_pix_function(uint32_t addr) { pix_function_ = addr; }

 protected:
  uint32_t pix_function_ = 0;

 private:
  std::unique_ptr<rex::thread::HighResolutionTimer> timestamp_timer_;
  // blur-nx (2026-11-13, M-38): the bundle (host pointer) and the timer started at its first import
  // (kernel_timestamp_timer_on_import) or at once.
  uint8_t* timestamp_bundle_ = nullptr;
  std::mutex timestamp_mutex_;
  void StartTimestampTimer();
};

}  // namespace rex::kernel::xboxkrnl
