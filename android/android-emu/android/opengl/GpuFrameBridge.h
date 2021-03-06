// Copyright (C) 2015 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

namespace android {

namespace base {
class Looper;
}  // namespace base

namespace opengl {

// EmuGL provides a way to send GPU frame data to its client, but will do
// so by calling a user-provided callback in an EmuGL-created thread. The
// corresponding data cannot be easily send to the UI, which expects to run
// on the process' main loop.
//
// GpuFrameBridge is a helper class that works-around this situation. Usage
// is the following:
//
//  1) Create a new GpuFrameBridge instance in the main loop thread, passing
//     a handle to the main loop's Looper instance, and the address of a
//     callback to retrieve frame data.
//
//  2) In the EmuGL callback, which runs in its own EmuGL thread, call the
//     postFrame() method.
//
class GpuFrameBridge {
public:
    // Type of function that is called to transfer the content of a new
    // GPU frame to the main thread. |opaque| is a user-provided pointer,
    // |width| and |height| are dimensions in pixels, and |pixels| is
    // the memory buffer of 32-bit RGBA image data. This buffer is freed
    // when the function returns.
    typedef void (Callback)(void* opaque,
                            int width,
                            int height,
                            const void* pixels);

    // Create a new GpuFrameBridge instance. |looper| is a handle to the main
    // loop's Looper instance, and |callback| is a function that will be
    // called, from the looper thread, to send the frame data to the client.
    static GpuFrameBridge* create(android::base::Looper* looper,
                                  Callback* callback,
                                  void* callbackOpaque);

    // Destructor
    virtual ~GpuFrameBridge() {}

    // Post a new frame from the EmuGL thread (synchronous).
    virtual void postFrame(int width, int height, const void* pixels) = 0;

    // Post callback (synchronous) specifically for recording purposes.
    // The intention is to either use this one or postFrame() as the callback
    // and not both, because the speed to allocate + copy the pixel data in
    // the postFrame() method is much too slow for recording.
    virtual void postRecordFrame(int width, int height, const void* pixels) = 0;

    // Async version of postRecordFrame for use with async readback.
    // Does not read the frame immediately.
    virtual void postRecordFrameAsync(int width, int height, const void* pixels) = 0;

    // Returns the currently displayed frame. This method is designed only for
    // recording. Returns null if there is no frame available. Make sure to
    // attach the postFrameRecording() as the callback or you will not get a
    // valid frame.
    virtual void* getRecordFrame() = 0;

    // Async version of getRecordFrame.
    virtual void* getRecordFrameAsync() = 0;

protected:
    GpuFrameBridge() {}
    GpuFrameBridge(const GpuFrameBridge& other);
};

}  // namespace opengl
}  // namespace android
