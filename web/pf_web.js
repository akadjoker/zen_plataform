// Browser side of the drop, fetch and clipboard support in backend_web.c. Files go to the MEMFS, then the
// C side raises EVENT_WINDOW_DROP through pf_web_drop_add / pf_web_drop_end.
//
// Threads: in a build with PROXY_TO_PTHREAD main() runs on a worker, where there is no window, no document and no
// navigator.clipboard. What touches the page is marked __proxy so that it runs on the browser's main thread
// ('sync': the caller waits; 'async': it does not, and must not pass memory it frees), and the C functions the listeners
// call (pf_web_paste, pf_web_copy_text, pf_web_flush_held) then run on that thread too: backend_web.c guards what they share.
mergeInto(LibraryManager.library, {
  // ---- clipboard ----------------------------------------------------------------------------------------------
  // The state of the listeners, in the JS realm of the main browser thread.
  $pfClip: {},

  // Ctrl or Meta with V, C, X and Shift+Insert, Ctrl+Insert, Shift+Delete are not prevented by the key callback, so the
  // browser fires paste, copy and cut: the only way to read the clipboard over plain http, and the one place where setting
  // it cannot be refused. Each handler hands the event to C and does the browser's half of it.
  pf_web_install_clipboard__proxy: 'sync',
  pf_web_install_clipboard__deps: ['$pfClip', '$stringToNewUTF8', '$UTF8ToString'],
  pf_web_install_clipboard: function() {
    var pf = pfClip;
    if (pf.installed)
      return;
    pf.installed = true;
    pf.pending = null; // text the browser refused to take outside of a gesture, tried again in the next one
    pf.inCopy = false;
    var mac = /Mac|iPhone|iPad|iPod/.test(navigator.platform || navigator.userAgent || '') ? 1 : 0;
    var capture = true;

    // a field of the page (an <input> next to the canvas) keeps its own paste, copy and cut
    var otherField = function(t) {
      return !!t && t.nodeType === 1 && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable);
    };

    // The browser says whether the keys make AltGr (Ctrl and Alt together on Windows layouts) only through this call,
    // and html5.h's key event has no such field: kept here for the key callback to ask (pf_web_altgr).
    window.addEventListener('keydown', function(e) {
      pf.altGr = !!(e.getModifierState && e.getModifierState('AltGraph'));
    }, capture);

    window.addEventListener('paste', function(e) {
      if (otherField(e.target))
        return;
      var cd = e.clipboardData, p = 0;
      if (cd && cd.getData && cd.types && Array.prototype.indexOf.call(cd.types, 'text/plain') >= 0)
        p = stringToNewUTF8(cd.getData('text/plain')); // C takes it over and frees it
      _pf_web_paste(p, mac);
      e.preventDefault();
    }, capture);

    var copyOrCut = function(cut) {
      return function(e) {
        if (otherField(e.target) || !e.clipboardData)
          return;
        // the application answers inside this call; what it sets with clipboard_set goes to the browser here, not through
        // the writes below (which would be a second copy inside the first)
        pf.inCopy = true;
        var p = 0;
        try { p = _pf_web_copy_text(cut, mac); } finally { pf.inCopy = false; }
        if (p) {
          e.clipboardData.setData('text/plain', UTF8ToString(p));
          e.preventDefault();
        }
      };
    };
    window.addEventListener('copy', copyOrCut(0), capture);
    window.addEventListener('cut', copyOrCut(1), capture);

    // The hidden field execCommand("copy") needs. Its focus and blur must not reach the window's focus callbacks (the
    // program would see its window lose the focus and take it back), and it must not take the focus for good.
    var legacyCopy = function(text) {
      var ta = pf.ta;
      if (!ta) {
        ta = pf.ta = document.createElement('textarea');
        ta.setAttribute('readonly', '');
        ta.setAttribute('aria-hidden', 'true');
        ta.tabIndex = -1;
        ta.style.cssText = 'position:fixed;top:0;left:0;width:1px;height:1px;padding:0;border:0;opacity:0;pointer-events:none;';
      }
      var prev = document.activeElement;
      document.body.appendChild(ta);
      ta.value = text;
      var ok = false;
      try {
        ta.focus({ preventScroll: true });
        ta.select();
        ta.setSelectionRange(0, text.length);
        ok = !!document.execCommand('copy');
      } catch (x) {}
      try { ta.blur(); } catch (x) {}
      document.body.removeChild(ta);
      if (prev && prev !== document.body && prev.focus) {
        try { prev.focus({ preventScroll: true }); } catch (x) {}
      }
      return ok;
    };
    ['focus', 'blur', 'focusin', 'focusout'].forEach(function(name) {
      window.addEventListener(name, function(e) {
        if (e.target === pf.ta)
          e.stopImmediatePropagation();
      }, capture);
    });

    // Where text goes to the browser's clipboard outside of a copy event: the async API when the page has it (a secure
    // context), else the textarea. Either can be refused when the call is not inside a gesture; then the text waits for the next.
    pf.write = function(text) {
      Module['pfClipLast'] = { text: text, inCopy: pf.inCopy, t: Date.now() }; // for a page that wants to see what was written
      if (pf.inCopy)
        return;
      pf.pending = null;
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).catch(function(err) {
          Module['pfClipError'] = String(err); // for a page that wants to know why the browser said no
          if (!legacyCopy(text))
            pf.pending = text;
        });
        return;
      }
      if (!legacyCopy(text))
        pf.pending = text;
    };
    var gesture = function(e) {
      if (pf.pending === null || !e.isTrusted)
        return;
      var text = pf.pending;
      pf.pending = null;
      pf.write(text);
    };
    ['pointerup', 'mouseup', 'touchend', 'keydown', 'keyup', 'click'].forEach(function(name) {
      window.addEventListener(name, gesture, capture);
    });
  },

  // the end of the task that a clipboard key came in: a held paste key whose paste event did not come is let go
  pf_web_defer_flush__proxy: 'sync',
  pf_web_defer_flush: function() {
    setTimeout(function() { _pf_web_flush_held(); }, 0);
  },

  pf_web_altgr__deps: ['$pfClip'],
  pf_web_altgr: function() {
    return pfClip.altGr ? 1 : 0;
  },

  // text for the browser's clipboard; the string is malloc'd by C and freed here
  pf_web_clipboard_write__proxy: 'async',
  pf_web_clipboard_write__deps: ['$pfClip', '$UTF8ToString', 'free'],
  pf_web_clipboard_write: function(ptr) {
    var text = UTF8ToString(ptr);
    _free(ptr);
    if (pfClip.write)
      pfClip.write(text);
  },

  pf_web_install_drop__proxy: 'sync',
  pf_web_install_drop__deps: ['$stringToNewUTF8', '$FS'],
  pf_web_install_drop: function() {
    // the whole page takes part, so a file dropped beside the canvas is not opened by the browser
    var c = window;
    var capture = true;
    c.addEventListener('dragenter', function(e) { e.preventDefault(); }, capture);
    c.addEventListener('dragover', function(e) {
      e.preventDefault();
      if (e.dataTransfer)
        e.dataTransfer.dropEffect = 'copy';
    }, capture);
    c.addEventListener('drop', function(e) {
      e.preventDefault();
      e.stopPropagation();
      var files = [];
      if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length)
        files = Array.prototype.slice.call(e.dataTransfer.files);
      else if (e.dataTransfer && e.dataTransfer.items)
        for (var n = 0; n < e.dataTransfer.items.length; n++) {
          var it = e.dataTransfer.items[n];
          var f0 = it.kind === 'file' ? it.getAsFile() : null;
          if (f0)
            files.push(f0);
        }
      if (!files.length)
        return;
      try { FS.mkdir('/drop'); } catch (x) {}
      var out = [];
      var next = function(i) {
        if (i >= files.length) {
          for (var k = 0; k < out.length; k++)
            _pf_web_drop_add(stringToNewUTF8(out[k]));
          _pf_web_drop_end();
          return;
        }
        var f = files[i];
        if (Module['onFileProgress'])
          Module['onFileProgress'](f.name, 0, f.size);
        f.arrayBuffer().then(function(buf) {
          var path = '/drop/' + f.name.split('/').join('_').split('\\').join('_');
          FS.writeFile(path, new Uint8Array(buf), { canOwn: true });
          out.push(path);
          if (Module['onFileProgress'])
            Module['onFileProgress'](f.name, f.size, f.size);
          next(i + 1);
        }, function(err) {
          if (Module['onFileError'])
            Module['onFileError'](f.name, String(err));
          next(i + 1);
        });
      };
      next(0);
    }, capture);
  },

  pf_web_download_js__deps: ['$FS', '$UTF8ToString'],
  pf_web_download_js: function(pathPtr, namePtr) {
    var path = UTF8ToString(pathPtr);
    var name = UTF8ToString(namePtr);
    var data;
    try { data = FS.readFile(path); } catch (x) { return 0; }
    var blob = new Blob([data], { type: 'application/octet-stream' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = name;
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    // the browser reads the Blob after the click returns: revoke a moment later
    setTimeout(function() {
      document.body.removeChild(a);
      URL.revokeObjectURL(url);
    }, 2000);
    if (Module['onDownload'])
      Module['onDownload'](name, data.length);
    return 1;
  },

  pf_web_fetch_js__deps: ['$stringToNewUTF8', '$FS', '$UTF8ToString'],
  pf_web_fetch_js: function(urlPtr, pathPtr) {
    var url = UTF8ToString(urlPtr);
    var path = UTF8ToString(pathPtr);
    var name = path.split('/').pop();
    var fail = function(msg) {
      if (Module['onFileError'])
        Module['onFileError'](name, msg);
    };
    fetch(url).then(function(r) {
      if (!r.ok) {
        fail('HTTP ' + r.status);
        return;
      }
      var total = parseInt(r.headers.get('content-length') || '0', 10);
      var chunks = [];
      var got = 0;
      var reader = r.body.getReader();
      var pump = function() {
        return reader.read().then(function(res) {
          if (res.done) {
            var data = new Uint8Array(got);
            var off = 0;
            for (var i = 0; i < chunks.length; i++) {
              data.set(chunks[i], off);
              off += chunks[i].length;
            }
            chunks = null;
            var parts = path.substring(0, path.lastIndexOf('/')).split('/');
            var cur = '';
            for (var j = 0; j < parts.length; j++) {
              if (!parts[j])
                continue;
              cur += '/' + parts[j];
              try { FS.mkdir(cur); } catch (x) {}
            }
            FS.writeFile(path, data, { canOwn: true });
            if (Module['onFileProgress'])
              Module['onFileProgress'](name, got, got);
            _pf_web_drop_add(stringToNewUTF8(path));
            _pf_web_drop_end();
            return;
          }
          chunks.push(res.value);
          got += res.value.length;
          if (Module['onFileProgress'])
            Module['onFileProgress'](name, got, total);
          return pump();
        });
      };
      return pump();
    }).catch(function(err) { fail(String(err)); });
  },
});
