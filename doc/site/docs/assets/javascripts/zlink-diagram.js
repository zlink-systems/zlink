/*
  다이어그램 iframe을 내용 높이에 맞춘다.

  그림마다 비율이 달라 높이를 하나로 정할 수 없다. 고정 높이를 주면 짧은 그림은 아래가
  비고 긴 그림은 잘린다. 다이어그램은 문서와 같은 출처에서 오므로 부모가 안쪽 문서의
  높이를 읽어 그대로 맞춘다.

  읽지 못하는 경우(출처가 다르거나 아직 그려지지 않은 경우)에는 extra.css의 기본 높이가
  남는다. 화면이 비지 않는다.
*/
(function () {
  "use strict";

  function fit(frame) {
    try {
      var doc = frame.contentDocument;
      if (!doc || !doc.body) return false;
      var el = doc.querySelector(".container") || doc.body;
      var h = Math.ceil(el.getBoundingClientRect().height);
      if (!h || h < 60) return false;
      frame.style.height = h + "px";
      return true;
    } catch (e) {
      return false;                      /*  접근이 막히면 기본 높이를 쓴다 */
    }
  }

  /*  archify는 그린 뒤에 viewBox를 맞추므로 한 번만 재면 이른 값을 얻는다.
      몇 차례 다시 재고, 값이 자리를 잡으면 멈춘다. */
  function settle(frame) {
    var tries = 0;
    var last = 0;
    (function again() {
      var ok = fit(frame);
      var now = parseInt(frame.style.height, 10) || 0;
      tries += 1;
      if (tries < 8 && (!ok || now !== last)) {
        last = now;
        setTimeout(again, 150 * tries);
      }
    })();
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
