<script>
(function () {
  var box = document.getElementById('zoom');
  var img = document.getElementById('zoom-img');
  var cap = document.getElementById('zoom-cap');
  var closeBtn = document.getElementById('zoom-close');
  var opener = null;
  function open(btn) {
    var src = btn.querySelector('img');
    var fig = btn.closest('figure');
    var fc = fig ? fig.querySelector('figcaption') : null;
    img.src = src.currentSrc || src.src;
    img.alt = src.alt;
    cap.textContent = fc ? fc.textContent : src.alt;
    opener = btn;
    box.hidden = false;
    document.documentElement.style.overflow = 'hidden';
    closeBtn.focus();
  }
  function close() {
    if (box.hidden) return;
    box.hidden = true;
    img.removeAttribute('src');
    document.documentElement.style.overflow = '';
    if (opener) opener.focus();
  }
  document.addEventListener('click', function (e) {
    var btn = e.target.closest('.shot');
    if (btn) { open(btn); return; }
    if (!box.hidden && (e.target === box || e.target === closeBtn || e.target === img)) close();
  });
  document.addEventListener('keydown', function (e) {
    if (box.hidden) return;
    if (e.key === 'Escape') { e.preventDefault(); close(); }
    else if (e.key === 'Tab') { e.preventDefault(); closeBtn.focus(); }
  });
})();
</script>
