#pragma once

#include "axmol.h"

class AppDelegate final : private ax::Application {
public:
  void initGLContextAttrs() override;
  bool applicationDidFinishLaunching() override;
  void applicationDidEnterBackground() override;
  void applicationWillEnterForeground() override;
};
