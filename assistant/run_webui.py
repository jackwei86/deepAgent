"""DeepAgent 集成启动器：Open WebUI + 素材库/工具说明 同进程同端口(8080)。

替代 `open-webui serve`：
  - 挂载 /media/api/*  素材库 API(复用 Open WebUI 登录鉴权)
  - 挂载 /media-ui/    素材库与工具说明单页
启动方式与官方 serve 一致(secret key 处理相同)，start.bat 调用本文件。
"""
import base64
import os
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def main() -> None:
    os.environ.setdefault("FROM_INIT_PY", "true")

    key_file = Path.cwd() / ".webui_secret_key"
    if os.getenv("WEBUI_SECRET_KEY") is None:
        if not key_file.exists():
            key_file.write_bytes(base64.b64encode(random.randbytes(12)))
        os.environ["WEBUI_SECRET_KEY"] = key_file.read_text()

    # 环境变量就绪后再导入 main(与官方 serve 顺序一致)
    import open_webui.main  # noqa: F401
    from fastapi.staticfiles import StaticFiles
    import uvicorn

    from assistant.media_api import router

    open_webui_app = open_webui.main.app

    # Open WebUI 在路由表末尾挂了 path 为 "" 的 SPA 兜底 Mount，
    # 后注册的路由永远不会命中。先注册素材库路由与页面，
    # 再把所有 SPA 兜底 Mount(path 为 "" 或 "/")挪到路由表最末尾。
    open_webui_app.include_router(router, prefix="/media/api")
    open_webui_app.mount(
        "/media-ui",
        StaticFiles(directory=str(ROOT / "assistant" / "media_ui"), html=True),
        name="media-ui",
    )

    for route in [r for r in open_webui_app.routes
                  if type(r).__name__ == "Mount" and getattr(r, "path", None) in ("", "/")]:
        open_webui_app.routes.remove(route)
        open_webui_app.routes.append(route)

    uvicorn.run(
        open_webui_app,
        host=os.environ.get("WEBUI_HOST", "127.0.0.1"),
        port=int(os.environ.get("WEBUI_PORT", "8080")),
        forwarded_allow_ips="*",
    )


if __name__ == "__main__":
    main()
