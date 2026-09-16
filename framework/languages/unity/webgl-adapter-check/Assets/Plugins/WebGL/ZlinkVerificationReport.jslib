// Result channel for the WebGL adapter check.
//
// This is the check's own plugin, not part of the adapter. It exists so the
// Playwright driver reads one value instead of scraping the console: a player
// that never starts leaves `zlinkVerificationResult` undefined, which is a
// different observation from a player that ran and reported a failure.
//
// It is also the control in the experiment. If this plugin is linked and the
// adapter's are not, the fault is in the adapter's plugin layout rather than in
// Unity's WebGL plugin pipeline.
mergeInto(LibraryManager.library, {

  ZlinkVerificationReport__deps: ['$UTF8ToString'],
  ZlinkVerificationReport: function (jsonPtr) {
    var text = UTF8ToString(jsonPtr);
    globalThis.zlinkVerificationResult = text;
    // Unity's default template overwrites document.title at startup only, so
    // this survives and is readable without evaluating page script.
    if (typeof document !== 'undefined') document.title = 'zlink-verification-done';
    console.log('ZLINK-VERIFY ' + text);
  },

  // A liveness channel for a run that never finishes. Without it a stalled
  // player is indistinguishable from a player that never started, and every
  // diagnosis costs another Unity build.
  ZlinkVerificationHeartbeat__deps: ['$UTF8ToString'],
  ZlinkVerificationHeartbeat: function (jsonPtr) {
    globalThis.zlinkVerificationHeartbeat = UTF8ToString(jsonPtr);
  },

  // Reports whether the adapter's own .jspre plugins reached the module scope.
  // The README's manual check "the built framework.js contains
  // ZlinkStreamWebGlRuntime and ZlinkStreamConnectorBundle" asked of the running
  // player instead of of the file, so a symbol that linked but landed in the
  // wrong closure is still caught.
  ZlinkVerificationLinkedPlugins: function () {
    var runtime = typeof ZlinkStreamWebGlRuntime !== 'undefined' && ZlinkStreamWebGlRuntime ? 1 : 0;
    var bundle = typeof ZlinkStreamConnectorBundle !== 'undefined' && ZlinkStreamConnectorBundle ? 2 : 0;
    return runtime | bundle;
  }
});
