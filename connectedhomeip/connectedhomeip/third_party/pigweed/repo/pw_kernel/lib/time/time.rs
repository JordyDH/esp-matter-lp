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
#![no_std]

use core::{
    marker::PhantomData,
    ops::{Add, Sub},
};

pub trait Clock: Sized {
    const TICKS_PER_SEC: u64;

    fn now() -> Instant<Self>;
}

pub struct Instant<Clock: crate::Clock> {
    ticks: u64,
    _phantom: PhantomData<Clock>,
}

impl<Clock: crate::Clock> Instant<Clock> {
    pub const MAX: Self = Self::from_ticks(u64::MAX);
    pub const MIN: Self = Self::from_ticks(u64::MIN);

    pub const fn from_ticks(ticks: u64) -> Self {
        Self {
            ticks,
            _phantom: PhantomData,
        }
    }

    pub const fn ticks(&self) -> u64 {
        self.ticks
    }

    pub const fn checked_add_duration(self, duration: Duration<Clock>) -> Option<Self> {
        if let Some(ticks) = self.ticks.checked_add_signed(duration.ticks) {
            Some(Self {
                ticks,
                _phantom: PhantomData,
            })
        } else {
            None
        }
    }

    pub const fn checked_sub_duration(self, duration: Duration<Clock>) -> Option<Self> {
        if let Some(ticks) = self.ticks.checked_add_signed(-duration.ticks) {
            Some(Self {
                ticks,
                _phantom: PhantomData,
            })
        } else {
            None
        }
    }
}

// Manually implement Copy so that we don't require `Clock` to be Copy
impl<Clock: crate::Clock> Copy for Instant<Clock> {}

// Manually implement Clone so that we don't require `Clock` to be Clone
impl<Clock: crate::Clock> Clone for Instant<Clock> {
    fn clone(&self) -> Self {
        *self
    }
}

// Manually implement Ord so that we don't require `Clock` to be Ord
impl<Clock: crate::Clock> Ord for Instant<Clock> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.ticks.cmp(&other.ticks)
    }
}

// Manually implement PartialOrd so that we don't require `Clock` to be PartialOrd
impl<Clock: crate::Clock> PartialOrd for Instant<Clock> {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.ticks.cmp(&other.ticks))
    }
}

// Manually implement Eq so that we don't require `Clock` to be Eq
impl<Clock: crate::Clock> Eq for Instant<Clock> {}

// Manually implement PartialEq so that we don't require `Clock` to be PartialEq
impl<Clock: crate::Clock> PartialEq for Instant<Clock> {
    fn eq(&self, other: &Self) -> bool {
        self.ticks == other.ticks
    }
}

impl<Clock: crate::Clock> Sub<Instant<Clock>> for Instant<Clock> {
    type Output = Duration<Clock>;

    fn sub(self, rhs: Instant<Clock>) -> Self::Output {
        Self::Output {
            // Use a wrapping_sub then conversion to i64 to avoid losing
            // resolution for large values of ticks.
            ticks: self.ticks.wrapping_sub(rhs.ticks) as i64,
            _phantom: PhantomData,
        }
    }
}

impl<Clock: crate::Clock> Add<Duration<Clock>> for Instant<Clock> {
    type Output = Instant<Clock>;

    fn add(self, rhs: Duration<Clock>) -> Self::Output {
        self.checked_add_duration(rhs)
            .expect("Instant - Duration overflow")
    }
}

impl<Clock: crate::Clock> Sub<Duration<Clock>> for Instant<Clock> {
    type Output = Instant<Clock>;

    fn sub(self, rhs: Duration<Clock>) -> Self::Output {
        self.checked_sub_duration(rhs)
            .expect("Instant - Duration overflow")
    }
}

pub struct Duration<Clock: crate::Clock> {
    ticks: i64,
    _phantom: PhantomData<Clock>,
}

impl<Clock: crate::Clock> Duration<Clock> {
    pub const MAX: Self = Self {
        ticks: i64::MAX,
        _phantom: PhantomData,
    };

    pub const MIN: Self = Self {
        ticks: i64::MIN,
        _phantom: PhantomData,
    };

    pub const fn ticks(self) -> i64 {
        self.ticks
    }

    pub const fn from_secs(secs: i64) -> Self {
        Self {
            ticks: secs * (Clock::TICKS_PER_SEC as i64),
            _phantom: PhantomData,
        }
    }

    pub const fn from_millis(millis: i64) -> Self {
        Self {
            ticks: millis * (Clock::TICKS_PER_SEC as i64) / 1000,
            _phantom: PhantomData,
        }
    }

    pub const fn from_micros(micros: i64) -> Self {
        Self {
            ticks: micros * (Clock::TICKS_PER_SEC as i64) / 1_000_000,
            _phantom: PhantomData,
        }
    }

    pub const fn from_nanos(nanos: i64) -> Self {
        Self {
            ticks: nanos * (Clock::TICKS_PER_SEC as i64) / 1_000_000_000,
            _phantom: PhantomData,
        }
    }

    pub const fn checked_add(self, rhs: Duration<Clock>) -> Option<Self> {
        if let Some(ticks) = self.ticks.checked_add(rhs.ticks) {
            Some(Self {
                ticks,
                _phantom: PhantomData,
            })
        } else {
            None
        }
    }

    pub const fn checked_sub(self, rhs: Duration<Clock>) -> Option<Self> {
        if let Some(ticks) = self.ticks.checked_sub(rhs.ticks) {
            Some(Self {
                ticks,
                _phantom: PhantomData,
            })
        } else {
            None
        }
    }
}

// Manually implement Copy so that we don't require `Duration` to be Copy
impl<Clock: crate::Clock> Copy for Duration<Clock> {}

// Manually implement Clone so that we don't require `Duration` to be Clone
impl<Clock: crate::Clock> Clone for Duration<Clock> {
    fn clone(&self) -> Self {
        *self
    }
}

// Manually implement Ord so that we don't require `Clock` to be Ord
impl<Clock: crate::Clock> Ord for Duration<Clock> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        self.ticks.cmp(&other.ticks)
    }
}

// Manually implement PartialOrd so that we don't require `Clock` to be PartialOrd
impl<Clock: crate::Clock> PartialOrd for Duration<Clock> {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.ticks.cmp(&other.ticks))
    }
}

// Manually implement Eq so that we don't require `Clock` to be Eq
impl<Clock: crate::Clock> Eq for Duration<Clock> {}

// Manually implement PartialEq so that we don't require `Clock` to be PartialEq
impl<Clock: crate::Clock> PartialEq for Duration<Clock> {
    fn eq(&self, other: &Self) -> bool {
        self.ticks == other.ticks
    }
}

impl<Clock: crate::Clock> Sub<Duration<Clock>> for Duration<Clock> {
    type Output = Duration<Clock>;

    fn sub(self, rhs: Duration<Clock>) -> Self::Output {
        self.checked_sub(rhs)
            .expect("Duration subtraction overflow")
    }
}

impl<Clock: crate::Clock> Add<Duration<Clock>> for Duration<Clock> {
    type Output = Duration<Clock>;

    fn add(self, rhs: Duration<Clock>) -> Self::Output {
        self.checked_add(rhs).expect("Duration addition overflow")
    }
}
