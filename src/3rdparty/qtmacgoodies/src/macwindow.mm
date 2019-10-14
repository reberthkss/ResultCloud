/*
 * Copyright (c) 2014, Markus Goetz <markus@woboq.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those
 * of the authors and should not be interpreted as representing official policies,
 * either expressed or implied, of the FreeBSD Project.
 */

#include "macwindow.h"

#include <QtMacExtras>
#include <QDebug>
#include <QWidget>
#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h> // private Qt header!

#import <Cocoa/Cocoa.h>
#import <Availability.h>

NSView *MacWindow::nsview(QWidget *w)
{
    // Code similar to getEmbeddableView() in QMacNativeWidget
    w->winId();
    w->windowHandle()->create();
    QPlatformNativeInterface *platformNativeInterface = QGuiApplication::platformNativeInterface();
    return (NSView *)platformNativeInterface->nativeResourceForWindow("nsview", w->windowHandle());
}

void MacWindow::bringToFront(QWidget *w) {
    Q_UNUSED(w);

    NSApplication *nsapp = [NSApplication sharedApplication];
    [nsapp activateIgnoringOtherApps:YES];

    NSView *view = nsview(w);
    if (!view) {
        return;
    }
    if (![view isKindOfClass:[NSView class]]) {
        return;
    }
    NSWindow *nswin = [view window];
    if ([nswin isKindOfClass:[NSWindow class]]) {
        [nswin makeKeyAndOrderFront:nswin];
        [nswin setOrderedIndex:0];
    }
}
