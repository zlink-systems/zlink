#include "AppDelegate.h"

#include "EngineLobbyScene.h"

void AppDelegate::initGLContextAttrs() {
  ax::GLView::setGLContextAttrs({8, 8, 8, 8, 24, 8, 0});
}

bool AppDelegate::applicationDidFinishLaunching() {
  auto *director = ax::Director::getInstance();
  auto *view =
      ax::GLViewImpl::createWithRect("Engine Lobby", ax::Rect(0, 0, 960, 540));
  director->setGLView(view);
  view->setDesignResolutionSize(960, 540, ax::ResolutionPolicy::SHOW_ALL);
  director->runWithScene(EngineLobbyScene::create());
  return true;
}

void AppDelegate::applicationDidEnterBackground() {
  ax::Director::getInstance()->stopAnimation();
}

void AppDelegate::applicationWillEnterForeground() {
  ax::Director::getInstance()->startAnimation();
}
