import json, urllib.request
url = "https://api.github.com/repos/gbdev/rgbds/releases"
req = urllib.request.Request(url, headers={"User-Agent": "gbcrecomp"})
with urllib.request.urlopen(req) as resp:
    releases = json.loads(resp.read().decode())
for r in releases[:5]:
    print(r['tag_name'])
    for a in r.get('assets', []):
        if 'win64' in a['name'].lower() or 'windows' in a['name'].lower():
            print(f"  {a['name']} -> {a['browser_download_url']}")
