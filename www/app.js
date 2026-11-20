fetch("/api/hello")
  .then((r) => r.text())
  .then((t) => { document.getElementById("out").textContent = "백엔드 응답: " + t; })
  .catch(() => { document.getElementById("out").textContent = "백엔드 응답: 없음 (백엔드가 꺼져 있음)"; });
