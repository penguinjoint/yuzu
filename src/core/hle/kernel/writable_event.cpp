// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "common/assert.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/object.h"
#include "core/hle/kernel/readable_event.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/writable_event.h"

namespace Kernel {

WritableEvent::WritableEvent(KernelCore& kernel) : Object{kernel} {}
WritableEvent::~WritableEvent() = default;

EventPair WritableEvent::CreateEventPair(KernelCore& kernel, std::string name) {
    std::shared_ptr<WritableEvent> writable_event(new WritableEvent(kernel));
    std::shared_ptr<ReadableEvent> readable_event(new ReadableEvent(kernel));

    writable_event->name = name + ":Writable";
    writable_event->readable = readable_event;
    readable_event->name = name + ":Readable";

    return {std::move(readable_event), std::move(writable_event)};
}

std::shared_ptr<ReadableEvent> WritableEvent::GetReadableEvent() const {
    return readable;
}

void WritableEvent::Signal() {
    readable->Signal();
}

void WritableEvent::Clear() {
    readable->Clear();
}

bool WritableEvent::IsSignaled() const {
    return readable->IsSignaled();
}

} // namespace Kernel
