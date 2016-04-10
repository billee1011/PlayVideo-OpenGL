//
// Created by Administrator on 2016/4/10 0010.
//

#include <stdint.h>
#include <jni.h>
#include <unistd.h>
#include <pthread.h>
#include <android/native_window.h> // requires ndk r5 or newer
#include <android/native_window_jni.h> // requires ndk r5 or newer
#include <EGL/egl.h> // requires ndk r5 or newer
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "NativeMedia.h"

#define LOG_TAG "NativeVideo"

#define LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)



static const GLfloat texMatrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, -1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
        1.0f, 1.0f, 0.0f, 1.0f
};

static float squareSize = 1.0f;
const GLfloat squareCoords[] = {
        -squareSize, squareSize,
        -squareSize, -squareSize,

        squareSize, -squareSize,
        squareSize, squareSize
};

static const GLfloat textureCoords[] = {
        0.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f
};

const GLint drawOrder[6] = {0, 1, 2, 0, 2, 3};

GLuint texID;
GLuint shaderProgram;
GLuint positionHandle;
GLint textureParamHandle;
GLuint textureCoordHandle;
GLuint textureTranformHandle;


static void checkGlError(const char *op) {
    for (GLint error = glGetError(); error; error = glGetError()) {
        LOG("after %s() glError (0x%x)\n", op, error);
    }
}

static const char gVertexShader[] =
        "attribute vec4 vPosition;\n"
                "attribute vec4 vTexCoordinate;\n"
                "uniform mat4 texMatrix;\n"
                "varying vec2 v_TexCoordinate;\n"
                "void main() {\n"
                "v_TexCoordinate = (texMatrix * vTexCoordinate).xy;\n"
                "gl_Position = vPosition;\n"
                "}\n";

static const char gFragmentShader[] =
        "#extension GL_OES_EGL_image_external : require\n"
                "precision mediump float;\n"
                "uniform samplerExternalOES  texture;\n"
                "varying vec2 v_TexCoordinate;\n"
                "void main() {\n"
                "vec4 color = texture2D(texture,v_TexCoordinate);\n"
                "gl_FragColor = color;\n"
                "}\n";

GLuint loadShader(GLenum shaderType, const char *pSource) {
    GLuint shader = glCreateShader(shaderType);
    if (shader) {
        glShaderSource(shader, 1, &pSource, NULL);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen) {
                char *buf = (char *) malloc(infoLen);
                if (buf) {
                    glGetShaderInfoLog(shader, infoLen, NULL, buf);
                    LOG("Could not compile shader %d:\n%s\n",
                        shaderType, buf);
                    free(buf);
                }
                glDeleteShader(shader);
                shader = 0;
            }
        }
    }
    return shader;
}

GLuint createProgram(const char *pVertexSource, const char *pFragmentSource) {
    GLuint vertexShader = loadShader(GL_VERTEX_SHADER, pVertexSource);
    if (!vertexShader) {
        return 0;
    }

    GLuint pixelShader = loadShader(GL_FRAGMENT_SHADER, pFragmentSource);
    if (!pixelShader) {
        return 0;
    }

    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertexShader);
        checkGlError("glAttachShader");
        glAttachShader(program, pixelShader);
        checkGlError("glAttachShader");
        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
            if (bufLength) {
                char *buf = (char *) malloc(bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, NULL, buf);
                    LOG("Could not link program:\n%s\n", buf);
                    free(buf);
                }
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    return program;
}

NativeMedia::NativeVideo():
        jni(NULL),
        javaVM(NULL),
        activity(0),
        nativeWindow(0),
        display(0),
        surface(0),
        context(0),
        textureId(0),
        javaObject(NULL),
        nanoTimeStamp(0),
        updateTexImageMethodId(NULL),
        getTimestampMethodId(NULL),
        setDefaultBufferSizeMethodId(NULL)
{
    pthread_create(&pthreadId, 0 , threadCallback, this);
}

NativeMedia::~NativeVideo() {
    if (javaObject) {
        jni->DeleteGlobalRef( javaObject );
        javaObject = 0;
    }
}

void *NativeMedia::threadCallback(void *myself) {
    NativeMedia *my = (NativeMedia *)myself;
    my->renderLoop();
    pthread_exit(0);
}

bool NativeMedia::setupGraphics(int w, int h) {
    shaderProgram = createProgram(gVertexShader, gFragmentShader);
    LOG("setupGraphics: %d  (w: %d)\n", shaderProgram, w);

    if (!shaderProgram) {
        LOG("gProgram: %d  (%s)\n", shaderProgram, "createProgram error");
        return false;
    }

    textureParamHandle = glGetUniformLocation(shaderProgram, "texture");
    checkGlError("glGetUniformLocation");
    LOG("glGetUniformLocation(\"texturen\") = %d\n", textureParamHandle);

    positionHandle = glGetAttribLocation(shaderProgram, "vPosition");
    checkGlError("glGetAttribLocation");
    LOG("glGetAttribLocation(\"positionHandle\") = %d\n", positionHandle);

    textureCoordHandle = glGetAttribLocation(shaderProgram, "vTexCoordinate");
    checkGlError("glGetAttribLocation");
    LOG("glGetAttribLocation(\"vTexCoordinater\") = %d\n", textureCoordHandle);

    textureTranformHandle = glGetUniformLocation(shaderProgram, "texMatrix");
    checkGlError("glGetAttribLocation");
    LOG("glGetUniformLocation(\"vtexMatrix\") = %d\n", textureTranformHandle);


    glGenTextures(1, &texID);
    checkGlError("glGenTextures");

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texID);
    checkGlError("glBindTexture");

    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    return true;
}


void NativeMedia::renderFrame() {

    glViewport(0, 0, 1920, 1080);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    checkGlError("glClearColor");
    glClear(GL_COLOR_BUFFER_BIT);
    checkGlError("glClear");

    glUseProgram(shaderProgram);
    checkGlError("glUseProgram");

    glVertexAttribPointer(positionHandle, 2, GL_FLOAT, GL_FALSE, 0, squareCoords);
    checkGlError("gQuadVertices,glVertexAttribPointer");
    glEnableVertexAttribArray(positionHandle);
    checkGlError("glEnableVertexAttribArray");

    glVertexAttribPointer(textureCoordHandle, 2, GL_FLOAT, GL_FALSE, 0, textureCoords);
    checkGlError("textureCoords,glVertexAttribPointer");
    glEnableVertexAttribArray(textureCoordHandle);
    checkGlError("glEnableVertexAttribArray");

    glUniformMatrix4fv(textureTranformHandle, 1, GL_FALSE, texMatrix);
    checkGlError("texMatrix, glUniformMatrix4fv");


    LOG("bindTextureId: %d  native_thread(%u)\n", texID, (unsigned int) pthread_self());

    glUniform1i(textureParamHandle, 0);
    checkGlError("glUniform1i");


    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, drawOrder);
    checkGlError("glDrawArrays");

}


void NativeMedia::renderLoop() {

    while (true) {

    }

}



bool NativeMedia::setupEGL () {
    const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_BLUE_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_RED_SIZE, 8,
            EGL_NONE
    };

    EGLint contextAttribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 3,
            EGL_NONE};

    EGLConfig config;
    EGLint numConfigs;
    EGLint format;
    EGLint width;
    EGLint height;
    //GLfloat ratio;

    LOG_INFO("Initializing context");

    if ((display = eglGetDisplay(EGL_DEFAULT_DISPLAY)) == EGL_NO_DISPLAY) {
        LOG_ERROR("eglGetDisplay() returned error %d", eglGetError());
        return false;
    }
    if (!eglInitialize(display, 0, 0)) {
        LOG_ERROR("eglInitialize() returned error %d", eglGetError());
        return false;
    }

    if (!eglChooseConfig(display, attribs, &config, 1, &numConfigs)) {
        LOG_ERROR("eglChooseConfig() returned error %d", eglGetError());
        destroy();
        return false;
    }

    if (!eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &format)) {
        LOG_ERROR("eglGetConfigAttrib() returned error %d", eglGetError());
        destroy();
        return false;
    }

    ANativeWindow_setBuffersGeometry(nativeWindow, 0, 0, format);

    if (!(surface = eglCreateWindowSurface(display, config, nativeWindow, 0))) {
        LOG_ERROR("eglCreateWindowSurface() returned error %d", eglGetError());
        destroy();
        return false;
    }

    if (!(context = eglCreateContext(display, config, 0, contextAttribs))) {
        LOG_ERROR("eglCreateContext() returned error %d", eglGetError());
        destroy();
        return false;
    }

    if (!eglMakeCurrent(display, surface, surface, context)) {
        LOG_ERROR("eglMakeCurrent() returned error %d", eglGetError());
        destroy();
        return false;
    }

    if (!eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &height)) {
        LOG_ERROR("eglQuerySurface() returned error %d", eglGetError());
        destroy();
        return false;
    }

    glDisable(GL_DITHER);
    glClearColor(0, 0, 0, 0);
    glDisable(GL_DEPTH_TEST);

    glViewport(0, 0, width, height);

    setupGraphics(width, height);

    return true;
}


//=================================================================================================//
//            native lifecycle
//=================================================================================================//

void NativeMedia::setNativeWindow(ANativeWindow *window) {

}
void NativeMedia::setActivity(jobject activityObj) {
    activity = activityObj;
}

void NativeMedia::destroy() {
    LOG_INFO("Destroying context");

    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
//    eglTerminate(diaplay);                ////  display not declare ????

    display = EGL_NO_DISPLAY;
    surface = EGL_NO_SURFACE;
    context = EGL_NO_CONTEXT;

}

JNIEnv*NativeMedia::AttachJava()
{
    JavaVMAttachArgs args = {JNI_VERSION_1_4, 0, 0};
    JNIEnv* java;
    javaVM->AttachCurrentThread( &java, &args);
    return java;
}


//==============================================================================================//
//              获取java SurfaceTexture
//==============================================================================================//

void NativeMedia::setupSurfaceTexture(int texId) {

    textureId = texId;
    jni = AttachJava();
    const char *stClassPath = "android/graphics/SurfaceTexture";
    const jclass surfaceTextureClass = jni->FindClass(stClassPath);
    if (surfaceTextureClass == 0) {
        LOG_ERROR("FindClass (%s) failed", stClassPath);
    }

//    // find the constructor that takes an int
    const jmethodID constructor = jni->GetMethodID( surfaceTextureClass, "<init>", "(I)V" );
    if (constructor == 0) {
        LOG_ERROR("GetMethonID(<init>) failed");
    }

    jobject  obj = jni->NewObject(surfaceTextureClass, constructor, texId);
    if (obj == 0) {
        LOG_ERROR("NewObject() failed");
    }

    javaObject = jni->NewGlobalRef(obj);
    if (javaObject == 0) {
        LOG_ERROR("NewGlobalRef() failed");
    }

    //Now that we have a globalRef, we can free the localRef
    jni->DeleteLocalRef(obj);

    updateTexImageMethodId = jni->GetMethodID( surfaceTextureClass, "updateTexImage", "()V");
    if ( !updateTexImageMethodId ) {
        LOG_ERROR("couldn't get updateTexImageMethonId");
    }

    getTimestampMethodId = jni->GetMethodID(surfaceTextureClass, "getTimestamp", "()J");
    if (!getTimestampMethodId) {
        LOG_ERROR("couldn't get TimestampMethodId");
    }

    // jclass objects are loacalRefs that need to be free;
    jni->DeleteLocalRef( surfaceTextureClass );

//    if(isAttached) javaVM->DetachCurrentThread();
    int textureID = GetTextureId();
    LOG_INFO("SurfaceTexture Init Success. JavaObject ptr:%p texId: %d ", &javaObject,GetTextureId());
    LOG_INFO("texId addr: %p ", &textureID);
}



void NativeMedia::SetDefaultBufferSizse(const int width, const int height) {
    jni->CallVoidMethod(javaObject, setDefaultBufferSizeMethodId, width, height);
}

void NativeMedia::Update() {
    //latch the latest movie frame to the texture
    if (!javaObject) {
        return;
    }
    LOG("+++++updateTexImage++++++javaObejct: %p *******textureId: %p",&javaObject, &textureId);
    jni->CallVoidMethod(javaObject, updateTexImageMethodId);
    nanoTimeStamp = jni->CallLongMethod( javaObject, getTimestampMethodId );
}

unsigned NativeMedia::GetTextureId() {
    return textureId;
}

jobject NativeMedia::GetJavaObject() {
    return javaObject;
}

long  long NativeMedia::GetNanoTimeStamp() {
    return nanoTimeStamp;
}



