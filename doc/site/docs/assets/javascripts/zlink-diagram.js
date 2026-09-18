/*
  다이어그램 iframe을 내용 높이에 맞춘다.

  그림마다 비율이 달라 높이를 하나로 정할 수 없다. 고정 높이를 주면 짧은 그림은 아래가
  비고 긴 그림은 잘린다. 다이어그램은 문서와 같은 출처에서 오므로 부모가 안쪽 문서의
  높이를 읽어 그대로 맞춘다.

  **다 그려지기 전에 재지 않는다.** archify는 SVG를 그린 뒤 viewBox를 맞추므로, 그리는
  도중에 재면 작은 값이 박혀 그림이 잘린다. 문서가 `complete`가 된 뒤에만 재고, 그 뒤로도
  안쪽 크기가 바뀌면 다시 잰다.

  재지 못하는 경우(출처가 다르거나 아직 열리지 않은 경우)에는 extra.css의 기본 높이가
  남는다. 화면이 비지 않는다.
*/
(function () {
  "use strict";

  var MIN = 160;                        /*  이보다 작게 재지면 아직 덜 그려진 것이다 */

  function innerBox(frame) {
    try {
      var doc = frame.contentDocument;
      if (!doc || doc.readyState !== "complete" || !doc.body) return null;
      /*  아직 열리지 않은 lazy iframe은 `about:blank`다. 빈 문서의 scrollHeight는
          iframe 자신의 높이를 돌려주므로 그럴듯한 값이 나온다. 그림이 실제로
          들어왔는지는 svg가 있는지로 판정한다. */
      if (!doc.querySelector("svg")) return null;
      return doc.querySelector(".container") || doc.body;
    } catch (e) {
      return null;                      /*  접근이 막히면 기본 높이를 쓴다 */
    }
  }

  function measure(frame) {
    var el = innerBox(frame);
    if (!el) return 0;
    var doc = el.ownerDocument;
    var h = Math.ceil(Math.max(
      el.getBoundingClientRect().height,
      doc.body.scrollHeight,
      doc.documentElement.scrollHeight));
    return h >= MIN ? h : 0;
  }

  function fit(frame) {
    var h = measure(frame);
    if (!h) return false;
    if (frame.style.height !== h + "px") frame.style.height = h + "px";
    return true;
  }

  /*  다 그려질 때까지 몇 차례 다시 잰다. 그린 뒤에는 안쪽 크기 변화를 따라간다. */
  function settle(frame) {
    var tries = 0;
    (function again() {
      var ok = fit(frame);
      tries += 1;
      if (!ok && tries < 240) {
        setTimeout(again, 250);         /*  화면 밖 iframe은 열릴 때까지 기다린다 */
        return;
      }
      if (!ok) return;
      observe(frame);
    })();
  }

  function observe(frame) {
    if (frame.zlinkObserved) return;
    var el = innerBox(frame);
    if (!el || typeof ResizeObserver !== "function") return;
    frame.zlinkObserved = true;
    try {
      new ResizeObserver(function () { fit(frame); }).observe(el);
    } catch (e) {
      frame.zlinkObserved = false;
    }
  }

  function attach(frame) {
    if (frame.getAttribute("data-zlink-fit") === "1") return;
    frame.setAttribute("data-zlink-fit", "1");
    frame.addEventListener("load", function () { settle(frame); });
    settle(frame);
  }

  function run() {
    var frames = document.querySelectorAll("iframe.zlink-diagram");
    Array.prototype.forEach.call(frames, attach);
  }

  var timer = null;
  window.addEventListener("resize", function () {
    if (timer) clearTimeout(timer);
    timer = setTimeout(function () {
      var frames = document.querySelectorAll("iframe.zlink-diagram");
      Array.prototype.forEach.call(frames, fit);
    }, 200);
  });

  /*  Material은 본문을 즉시 교체하므로(instant loading) 이동마다 다시 건다. */
  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(run);
  } else {
    document.addEventListener("DOMContentLoaded", run);
  }
})();
