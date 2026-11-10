// SPDX-License-Identifier: Apache-2.0
#pragma once

// Call once in setup(), before Edge::instance().init().
void trailerSetup();

// Call in loop(), on the same thread as Edge::instance().loop().
void trailerLoop();
