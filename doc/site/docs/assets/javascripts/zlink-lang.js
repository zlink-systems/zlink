/*
  언어 선택기와 사이드바 평탄화.

  nav의 최상위는 언어다(`mkdocs.yml`의 `- .NET:` 이하 다섯). 그대로 두면 사이드바
  1차 메뉴가 언어가 되고, 독자가 찾는 "시작·기능별 가이드·동작 원리"는 2차로 밀린다.
  그래서 지금 언어의 한 겹을 걷어 그 안의 묶음이 1차가 되게 하고, 나머지 넷은 감춘다.
  nav 자체는 건드리지 않으므로 이전·다음 장 링크와 활성 표시가 그대로 동작한다.

  Spec·Bindings·Core·Samples는 언어가 아니므로 손대지 않는다. 1차 메뉴에 그대로 남는다.

  언어는 주소에서 읽는다. `/ko/dotnet/guide/server/20-…` 또는 `/dotnet/…` 꼴이고,
  로케일 구간(`ko`)은 있을 수도 없을 수도 있다. 가이드 밖 문서를 보고 있으면 주소에
  언어가 없으므로 마지막으로 고른 값을 사용하고, 선택기는 감춘다.
*/
(function () {
  "use strict";

  var LANGS = ["cpp", "dotnet", "java", "kotlin", "node"];
  /*  mkdocs-static-i18n의 로케일. 기본(en)은 주소에 구간이 없다. */
  var LOCALES = ["ko", "en"];
  var KEY = "zlink-lang";

  function stored() {
    try {
      var v = localStorage.getItem(KEY);
      return LANGS.indexOf(v) >= 0 ? v : null;
    } catch (e) {
      return null;
    }
  }

  function remember(lang) {
    try {
      localStorage.setItem(KEY, lang);
    } catch (e) {
      /*  사생활 보호 창에서는 저장이 막힌다. 화면은 그대로 돈다. */
    }
  }

  /*  경로 조각에서 언어 구간의 자리. 없으면 -1.

      언어는 **로케일 바로 다음 구간**일 때만 인정한다. `bindings/guide/dotnet/…`처럼
      안쪽에 언어 이름이 들어 있는 경로가 있어, 아무 자리나 찾으면 Bindings가 `.NET`
      묶음으로 잘못 판별된다. */
  function langIndex(parts) {
    var i = (parts.length && LOCALES.indexOf(parts[0]) >= 0) ? 1 : 0;
    return (parts.length > i && LANGS.indexOf(parts[i]) >= 0) ? i : -1;
  }

  function pathLang() {
    var parts = location.pathname.split("/").filter(Boolean);
    var i = langIndex(parts);
    return i >= 0 ? parts[i] : null;
  }

  /*  같은 장의 다른 언어 주소. 지금 주소에 언어 구간이 없으면 null. */
  function swapped(lang) {
    var parts = location.pathname.split("/").filter(Boolean);
    var i = langIndex(parts);
    if (i < 0) return null;
    parts[i] = lang;
    return "/" + parts.join("/") + "/";
  }

  function topItems() {
    var list = document.querySelector(".md-nav--primary > .md-nav__list");
    if (!list) return [];
    return Array.prototype.slice.call(list.children).filter(function (el) {
      return el.classList && el.classList.contains("md-nav__item");
    });
  }

  /*  그 최상위 항목이 어느 언어의 묶음인지. 언어 묶음이 아니면 null.

      지금 보는 언어의 항목은 상대 링크(`../01-overview/`)로 렌더되어 경로에 언어
      구간이 없다. 절대 경로로 풀어야 판별된다. */
  function itemLang(item) {
    var links = item.querySelectorAll("a.md-nav__link[href]");
    for (var n = 0; n < links.length; n++) {
      var href = links[n].getAttribute("href");
      if (!href || href.charAt(0) === "#") continue;
      var path;
      try {
        path = new URL(href, location.href).pathname;
      } catch (e) {
        continue;
      }
      var parts = path.split("/").filter(Boolean);
      var i = langIndex(parts);
      if (i >= 0) return parts[i];
    }
    return null;
  }

  function apply(lang) {
    topItems().forEach(function (item) {
      var l = itemLang(item);
      if (!l) return;                       /*  Spec·Bindings·Core는 그대로 둔다 */
      var mine = l === lang;
      item.classList.toggle("zlink-lang-current", mine);
      item.hidden = !mine;
      if (mine) {
        /*  걷어낸 겹의 토글이 접힌 채면 안쪽이 숨는다. 펴 둔다. */
        var toggle = item.querySelector(":scope > .md-nav__toggle");
        if (toggle) toggle.checked = true;
      }
    });
  }

  function fillSelector(lang) {
    var box = document.querySelector(".zlink-lang");
    if (!box) return;
    var links = box.querySelectorAll("a[data-zlink-lang]");
    var any = false;
    var label = "";
    Array.prototype.forEach.call(links, function (a) {
      var value = a.getAttribute("data-zlink-lang");
      var target = swapped(value);
      if (target) {
        a.setAttribute("href", target);
        any = true;
      }
      if (value === lang) label = a.textContent;
      a.parentNode.hidden = value === lang;
    });
    /*  버튼에 지금 언어의 이름을 적는다. 아이콘만 두면 무엇을 고르는 자리인지
        처음 보는 독자가 알 수 없다. */
    var name = box.querySelector(".zlink-lang__label");
    if (name) name.textContent = label;
    box.hidden = !any;                      /*  가이드 밖에서는 선택기를 감춘다 */
  }

  /*  지금 주소의 로케일. 기본(en)은 접두사가 없다. */
  function currentLocale() {
    var parts = location.pathname.split("/").filter(Boolean);
    return (parts.length && LOCALES.indexOf(parts[0]) >= 0) ? parts[0] : "en";
  }

  /*  같은 문서의 다른 로케일 주소. */
  function localeUrl(locale) {
    var parts = location.pathname.split("/").filter(Boolean);
    if (parts.length && LOCALES.indexOf(parts[0]) >= 0) parts.shift();
    if (locale !== "en") parts.unshift(locale);
    return "/" + (parts.length ? parts.join("/") + "/" : "");
  }

  /*  로케일 전환기(한국어/English)의 링크를 다시 계산한다.

      instant 로딩에서는 헤더가 다시 그려지지 않아, 빌드 때 박힌 링크가 처음 연 문서의
      것으로 굳는다. 이동마다 지금 주소로 다시 맞춘다. */
  function fixLocaleLinks() {
    var links = document.querySelectorAll(".md-select__link[hreflang]");
    Array.prototype.forEach.call(links, function (a) {
      var locale = a.getAttribute("hreflang");
      if (LOCALES.indexOf(locale) < 0) return;
      a.setAttribute("href", localeUrl(locale));
    });
  }

  function run() {
    var lang = pathLang() || stored() || "dotnet";
    if (pathLang()) remember(lang);
    apply(lang);
    fillSelector(lang);
    fixLocaleLinks();
  }

  /*  Material은 본문을 즉시 교체하므로(instant loading) 이동마다 다시 건다. */
  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(run);
  } else {
    document.addEventListener("DOMContentLoaded", run);
  }
})();
