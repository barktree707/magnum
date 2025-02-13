/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018
              Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2018, 2019 Jonathan Hale <squareys@googlemail.com>
    Copyright © 2020 Pablo Escobar <mail@rvrs.in>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include <Corrade/Utility/Arguments.h>

#include "Magnum/Platform/EmscriptenApplication.h"
#include "Magnum/GL/Renderer.h"
#include "Magnum/GL/DefaultFramebuffer.h"
#include "Magnum/GL/Mesh.h"
#include "Magnum/Math/Color.h"
#include "Magnum/Math/ConfigurationValue.h"

/* The __EMSCRIPTEN_major__ etc macros used to be passed implicitly, version
   3.1.4 moved them to a version header and version 3.1.23 dropped the
   backwards compatibility. To work consistently on all versions, including the
   header only if the version macros aren't present.
   https://github.com/emscripten-core/emscripten/commit/f99af02045357d3d8b12e63793cef36dfde4530a
   https://github.com/emscripten-core/emscripten/commit/f76ddc702e4956aeedb658c49790cc352f892e4c */
#if defined(CORRADE_TARGET_EMSCRIPTEN) && !defined(__EMSCRIPTEN_major__)
#include <emscripten/version.h>
#endif

namespace Magnum { namespace Platform { namespace Test {

using namespace Containers::Literals;
using namespace Math::Literals;

struct EmscriptenApplicationTest: Platform::Application {
    /* For testing resize events */
    explicit EmscriptenApplicationTest(const Arguments& arguments);

    virtual void drawEvent() override {
        Debug() << "draw event";
        #ifdef CUSTOM_CLEAR_COLOR
        GL::Renderer::setClearColor(CUSTOM_CLEAR_COLOR);
        #endif
        GL::defaultFramebuffer.clear(GL::FramebufferClear::Color);

        swapBuffers();

        if(_redraw) {
            redraw();
        }
    }

    #ifdef MAGNUM_TARGET_GL
    /* For testing HiDPI resize events */
    void viewportEvent(ViewportEvent& event) override {
        Debug{} << "viewport event" << event.windowSize() << event.framebufferSize() << event.dpiScaling() << event.devicePixelRatio();
    }
    #endif

    /* For testing event coordinates */
    void mousePressEvent(MouseEvent& event) override {
        Debug{} << "mouse press event:" << event.position() << Int(event.button());
    }

    void mouseReleaseEvent(MouseEvent& event) override {
        Debug{} << "mouse release event:" << event.position() << Int(event.button());
    }

    void mouseMoveEvent(MouseMoveEvent& event) override {
        Debug{} << "mouse move event:" << event.position() << event.relativePosition() << Int(event.buttons());
    }

    void mouseScrollEvent(MouseScrollEvent& event) override {
        Debug{} << "mouse scroll event:" << event.offset() << event.position();
    }

    /* For testing keyboard capture */
    void keyPressEvent(KeyEvent& event) override {
        {
            Debug d;
            if(event.key() != KeyEvent::Key::Unknown) {
                d << "keyPressEvent(" << Debug::nospace << event.keyName().data() << Debug::nospace << "): ✔";
            } else {
                d << "keyPressEvent(" << Debug::nospace << event.keyName().data() << Debug::nospace << "): ✘";
            }

            if(event.modifiers() & KeyEvent::Modifier::Shift) d << "Shift";
            if(event.modifiers() & KeyEvent::Modifier::Ctrl) d << "Ctrl";
            if(event.modifiers() & KeyEvent::Modifier::Alt) d << "Alt";
            if(event.modifiers() & KeyEvent::Modifier::Super) d << "Super";
        }

        if(event.key() == KeyEvent::Key::F1) {
            Debug{} << "starting text input";
            startTextInput();
        } else if(event.key() == KeyEvent::Key::F2) {
            _redraw = !_redraw;
            Debug{} << "redrawing" << (_redraw ? "enabled" : "disabled");
            if(_redraw) redraw();
        } else if(event.key() == KeyEvent::Key::Esc) {
            Debug{} << "stopping text input";
            stopTextInput();
        } else if(event.key() == KeyEvent::Key::F) {
            Debug{} << "toggling fullscreen";
            setContainerCssClass((_fullscreen ^= true) ? "mn-fullsizeX"_s.exceptSuffix(1) : "");
        } else if(event.key() == KeyEvent::Key::T) {
            Debug{} << "setting window title";
            setWindowTitle("This is a UTF-8 Window Title™ and it should have no exclamation mark!!"_s.exceptSuffix(2));
        } else if(event.key() == KeyEvent::Key::H) {
            Debug{} << "toggling hand cursor";
            setCursor(cursor() == Cursor::Arrow ? Cursor::Hand : Cursor::Arrow);
        }

        event.setAccepted();
    }

    void keyReleaseEvent(KeyEvent& event) override {
        {
            Debug d;
            if(event.key() != KeyEvent::Key::Unknown) {
                d << "keyReleaseEvent(" << Debug::nospace << event.keyName() << Debug::nospace << "): ✔";
            } else {
                d << "keyReleaseEvent(" << Debug::nospace << event.keyName() << Debug::nospace << "): ✘";
            }

            if(event.modifiers() & KeyEvent::Modifier::Shift) d << "Shift";
            if(event.modifiers() & KeyEvent::Modifier::Ctrl) d << "Ctrl";
            if(event.modifiers() & KeyEvent::Modifier::Alt) d << "Alt";
            if(event.modifiers() & KeyEvent::Modifier::Super) d << "Super";
        }

        event.setAccepted();
    }

    void textInputEvent(TextInputEvent& event) override {
        Debug{} << "text input event:" << event.text();

        event.setAccepted();
    }

    private:
        bool _fullscreen = false;
        bool _redraw = false;
};

EmscriptenApplicationTest::EmscriptenApplicationTest(const Arguments& arguments): Platform::Application{arguments, NoCreate} {
    Utility::Arguments args;
    args.addOption("dpi-scaling").setHelp("dpi-scaling", "DPI scaled passed via Configuration instead of --magnum-dpi-scaling, to test app overrides")
        .addSkippedPrefix("magnum", "engine-specific options")
        .addBooleanOption("exit-immediately").setHelp("exit-immediately", "exit the application immediately from the constructor, to test that the app doesn't run any event handlers after")
        .addBooleanOption("quiet").setHelp("quiet", "like --magnum-log quiet, but specified via a Context::Configuration instead")
        .parse(arguments.argc, arguments.argv);

    /* Useful for bisecting Emscripten regressions, because they happen WAY TOO
       OFTEN!!! */
    Debug{} << "Emscripten version:"
        << __EMSCRIPTEN_major__ << Debug::nospace << "." << Debug::nospace
        << __EMSCRIPTEN_minor__ << Debug::nospace << "." << Debug::nospace
        << __EMSCRIPTEN_tiny__ << Debug::nospace;

    if(args.isSet("exit-immediately")) {
        exit();
        return;
    }

    Configuration conf;
    conf.setWindowFlags(Configuration::WindowFlag::Resizable);
    if(!args.value("dpi-scaling").empty())
        conf.setSize({640, 480}, args.value<Vector2>("dpi-scaling"));
    GLConfiguration glConf;
    if(args.isSet("quiet"))
        glConf.addFlags(GLConfiguration::Flag::QuietLog);
    /* No GL-specific verbose log in EmscriptenApplication that we'd need to
       handle explicitly */
    /* No GPU validation on WebGL */
    create(conf, glConf);

    Debug{} << "window size" << windowSize()
        #ifdef MAGNUM_TARGET_GL
        << framebufferSize()
        #endif
        << dpiScaling() << devicePixelRatio();

    /* This uses a VAO on WebGL 1, so it will crash in case GL flags are
       missing EnableExtensionsByDefault (uncomment above) */
    GL::Mesh mesh;
}

}}}

MAGNUM_APPLICATION_MAIN(Magnum::Platform::Test::EmscriptenApplicationTest)
