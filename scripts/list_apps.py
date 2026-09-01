import urllib.request
import json
import ftplib

ftp = ftplib.FTP()
ftp.connect('192.168.0.6', 2121)
ftp.login()

for base in ['/system/vsh/app', '/system_ex/app', '/user/app']:
    try:
        lines = []
        ftp.dir(base, lines.append)
        print(f"=== {base} ===")
        for line in lines:
            parts = line.split()
            if not parts:
                continue
            app = parts[-1]
            if app in ['.', '..']:
                continue
            param_path = f"{base}/{app}/sce_sys/param.json"
            try:
                data = bytearray()
                ftp.retrbinary(f"RETR {param_path}", data.extend)
                j = json.loads(data.decode('utf-8'))
                title = j.get('localizedParameters', {}).get('en-US', {}).get('titleName', 'Unknown')
                print(f"  {app:12} : {title}")
            except Exception:
                pass
    except Exception as e:
        print(f"Error {base}: {e}")

ftp.quit()
