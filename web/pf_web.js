// Browser side of the drop and fetch support in backend_web.c. Files go to the MEMFS, then the
// C side raises EVENT_WINDOW_DROP through pf_web_drop_add / pf_web_drop_end.
mergeInto(LibraryManager.library, {
  pf_web_install_drop__deps: ['$stringToNewUTF8', '$FS'],
  pf_web_install_drop: function() {
    var c = Module['canvas'];
    if (!c)
      return;
    c.addEventListener('dragenter', function(e) { e.preventDefault(); });
    c.addEventListener('dragover', function(e) {
      e.preventDefault();
      if (e.dataTransfer)
        e.dataTransfer.dropEffect = 'copy';
    });
    c.addEventListener('drop', function(e) {
      e.preventDefault();
      var files = e.dataTransfer ? Array.prototype.slice.call(e.dataTransfer.files) : [];
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
    });
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
