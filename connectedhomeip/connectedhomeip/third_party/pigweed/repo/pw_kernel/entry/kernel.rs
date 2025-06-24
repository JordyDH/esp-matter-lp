// Copyright 2025 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#![no_main]
#![no_std]

// TODO: move this entry point into the arch module, possibly.

// Panic handler that halts the CPU on panic.
use console_backend as _;
use target as _;

// Cortex-M runtime entry macro.
#[cfg(feature = "arch_arm_cortex_m")]
use cortex_m_rt::entry;

// RISCV runtime entry maco.
#[cfg(feature = "arch_riscv")]
use riscv_rt::entry;

use kernel::Kernel;

#[entry]
fn main() -> ! {
    Kernel::main();
}
