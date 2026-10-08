from pathlib import Path
raw=Path('web/index.html').read_bytes()
Path('src/webui.h').write_text('#pragma once\nstatic const unsigned char webui[] = {'+','.join(str(x) for x in raw)+'};\nstatic const unsigned long webui_len = sizeof(webui);\n')
