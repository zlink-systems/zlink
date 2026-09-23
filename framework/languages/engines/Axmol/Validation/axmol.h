#pragma once

#include <string>

#define CREATE_FUNC(T)                                                         \
  static T *create() { return new T; }
#define AXLOG(...) (void)0

namespace ax {
struct Size {
  float width = 960;
  float height = 540;
};
struct Vec2 {
  Vec2(float, float) {}
};
struct Rect {
  Rect(float, float, float, float) {}
};
enum class ResolutionPolicy { SHOW_ALL };
struct GLContextAttrs {
  int red, green, blue, alpha, depth, stencil, multisamples;
};
class GLView {
public:
  static void setGLContextAttrs(GLContextAttrs) {}
  void setDesignResolutionSize(float, float, ResolutionPolicy) {}
};
class GLViewImpl : public GLView {
public:
  static GLViewImpl *createWithRect(const char *, Rect) { return nullptr; }
};
class Director {
public:
  static Director *getInstance() { return nullptr; }
  Size getVisibleSize() const { return {}; }
  void setGLView(GLView *) {}
  void runWithScene(class Scene *) {}
  void stopAnimation() {}
  void startAnimation() {}
};
class Label {
public:
  static Label *createWithSystemFont(const char *, const char *, int) {
    return nullptr;
  }
  void setPosition(Vec2) {}
  void setString(const std::string &) {}
};
class Scene {
public:
  virtual ~Scene() = default;
  virtual bool init() { return true; }
  virtual void onExit() {}
  void addChild(Label *) {}
  template <typename F> void schedule(F, const char *) {}
  void unschedule(const char *) {}
};
class Application {
public:
  virtual ~Application() = default;
  virtual void initGLContextAttrs() {}
  virtual bool applicationDidFinishLaunching() { return true; }
  virtual void applicationDidEnterBackground() {}
  virtual void applicationWillEnterForeground() {}
  static Application *getInstance() { return nullptr; }
  int run() { return 0; }
};
} // namespace ax
