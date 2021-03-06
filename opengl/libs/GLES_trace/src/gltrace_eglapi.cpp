/*
 * Copyright 2011, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include <stdlib.h>
#include <cutils/log.h>
#include <cutils/properties.h>

#include "hooks.h"
#include "glestrace.h"

#include "gltrace_context.h"
#include "gltrace_egl.h"
#include "gltrace_hooks.h"
#include "gltrace_transport.h"

namespace android {

using gltrace::GLTraceState;
using gltrace::GLTraceContext;
using gltrace::TCPStream;

static GLTraceState *sGLTraceState;
static pthread_t sReceiveThreadId;

/**
 * Task that monitors the control stream from the host and updates
 * the trace status according to commands received from the host.
 */
static void *commandReceiveTask(void *arg) {
    GLTraceState *state = (GLTraceState *)arg;
    TCPStream *stream = state->getStream();

    // The control stream always receives an integer size of the
    // command buffer, followed by the actual command buffer.
    uint32_t cmdSize;

    // Command Buffer
    void *cmdBuf = NULL;
    uint32_t cmdBufSize = 0;

    enum TraceSettingsMasks {
        READ_FB_ON_EGLSWAP_MASK = 1 << 0,
        READ_FB_ON_GLDRAW_MASK = 1 << 1,
        READ_TEXTURE_DATA_ON_GLTEXIMAGE_MASK = 1 << 2,
    };

    while (true) {
        // read command size
        if (stream->receive(&cmdSize, sizeof(uint32_t)) < 0) {
            break;
        }
        cmdSize = ntohl(cmdSize);

        // ensure command buffer is of required size
        if (cmdBufSize < cmdSize) {
            free(cmdBuf);
            cmdBufSize = cmdSize;
            cmdBuf = malloc(cmdSize);
            if (cmdBuf == NULL)
                break;
        }

        // receive the command
        if (stream->receive(cmdBuf, cmdSize) < 0) {
            break;
        }

        if (cmdSize != sizeof(uint32_t)) {
            // Currently, we only support commands that are a single integer,
            // so we skip all other commands
            continue;
        }

        uint32_t cmd = ntohl(*(uint32_t*)cmdBuf);

        bool collectFbOnEglSwap = (cmd & READ_FB_ON_EGLSWAP_MASK) != 0;
        bool collectFbOnGlDraw = (cmd & READ_FB_ON_GLDRAW_MASK) != 0;
        bool collectTextureData = (cmd & READ_TEXTURE_DATA_ON_GLTEXIMAGE_MASK) != 0;

        state->setCollectFbOnEglSwap(collectFbOnEglSwap);
        state->setCollectFbOnGlDraw(collectFbOnGlDraw);
        state->setCollectTextureDataOnGlTexImage(collectTextureData);

        ALOGD("trace options: eglswap: %d, gldraw: %d, texImage: %d",
            collectFbOnEglSwap, collectFbOnGlDraw, collectTextureData);
    }

    ALOGE("Stopping OpenGL Trace Command Receiver\n");

    free(cmdBuf);
    return NULL;
}

void GLTrace_start() {
    char udsName[PROPERTY_VALUE_MAX];

    property_get("debug.egl.debug_portname", udsName, "gltrace");
    int clientSocket = gltrace::acceptClientConnection(udsName);
    if (clientSocket < 0) {
        ALOGE("Error creating GLTrace server socket. Quitting application.");
        exit(-1);
    }

    // create communication channel to the host
    TCPStream *stream = new TCPStream(clientSocket);

    // initialize tracing state
    sGLTraceState = new GLTraceState(stream);

    pthread_create(&sReceiveThreadId, NULL, commandReceiveTask, sGLTraceState);
}

void GLTrace_stop() {
    delete sGLTraceState;
    sGLTraceState = NULL;
}

void GLTrace_eglCreateContext(int version, EGLContext c) {
    // update trace state for new EGL context
    GLTraceContext *traceContext = sGLTraceState->createTraceContext(version, c);
    gltrace::setupTraceContextThreadSpecific(traceContext);

    // trace command through to the host
    gltrace::GLTrace_eglCreateContext(version, traceContext->getId());
}

void GLTrace_eglMakeCurrent(const unsigned version, gl_hooks_t *hooks, EGLContext c) {
    // setup per context state
    GLTraceContext *traceContext = sGLTraceState->getTraceContext(c);
    traceContext->hooks = hooks;
    gltrace::setupTraceContextThreadSpecific(traceContext);

    // trace command through to the host
    gltrace::GLTrace_eglMakeCurrent(traceContext->getId());
}

void GLTrace_eglReleaseThread() {
    gltrace::releaseContext();
}

void GLTrace_eglSwapBuffers(void *dpy, void *draw) {
    gltrace::GLTrace_eglSwapBuffers(dpy, draw);
}

gl_hooks_t *GLTrace_getGLHooks() {
    return gltrace::getGLHooks();
}

}
