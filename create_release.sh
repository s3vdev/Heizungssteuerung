#!/bin/bash

# GitHub Release Script für v2.2.1

REPO="s3vdev/Heizungssteuerung"
TAG="v2.2.1"
ZIP_FILE="ESP32-Heizungssteuerung-v2.2.1.zip"
RELEASE_NOTES="RELEASE_NOTES_v2.2.1.md"

echo "📦 Erstelle GitHub Release für $TAG..."

# Prüfe ob GitHub CLI installiert ist
if command -v gh &> /dev/null; then
    echo "✅ GitHub CLI gefunden"
    
    # Erstelle Release mit gh
    gh release create "$TAG" \
        "$ZIP_FILE" \
        --title "Release v2.2.1" \
        --notes-file "$RELEASE_NOTES" \
        --repo "$REPO"
    
    echo "✅ Release erstellt!"
elif [ -n "$GITHUB_TOKEN" ]; then
    echo "✅ GitHub Token gefunden - erstelle Release via API..."
    
    # Erstelle Release via API (macOS-safe JSON encoding)
    # Avoid BSD sed incompatibilities and quoting issues by generating the full JSON payload in Python.
    CREATE_PAYLOAD=$(python3 - <<PY
import json
with open("$RELEASE_NOTES", "r", encoding="utf-8") as f:
    body = f.read()
payload = {
  "tag_name": "$TAG",
  "name": "Release $TAG",
  "body": body,
  "draft": False,
  "prerelease": False
}
print(json.dumps(payload))
PY
)

    RESPONSE=$(curl -s -X POST \
        -H "Authorization: token $GITHUB_TOKEN" \
        -H "Accept: application/vnd.github.v3+json" \
        "https://api.github.com/repos/$REPO/releases" \
        -d "$CREATE_PAYLOAD")
    
    # Extract upload_url (macOS-safe)
    UPLOAD_URL=$(python3 - <<PY
import json, sys
raw = sys.stdin.read().strip()
if not raw:
    print("")
    sys.exit(0)
try:
    data = json.loads(raw)
except Exception:
    print("")
    sys.exit(0)
url = data.get("upload_url", "") or ""
print(url.replace("{?name,label}", ""))
PY
<<<"$RESPONSE")

    # If release already exists (or POST failed), fetch existing release by tag and retry getting upload_url
    if [ -z "$UPLOAD_URL" ]; then
        RESPONSE=$(curl -s -X GET \
            -H "Authorization: token $GITHUB_TOKEN" \
            -H "Accept: application/vnd.github.v3+json" \
            "https://api.github.com/repos/$REPO/releases/tags/$TAG")
        UPLOAD_URL=$(python3 - <<PY
import json, sys
raw = sys.stdin.read().strip()
if not raw:
    print("")
    sys.exit(0)
try:
    data = json.loads(raw)
except Exception:
    print("")
    sys.exit(0)
url = data.get("upload_url", "") or ""
print(url.replace("{?name,label}", ""))
PY
<<<"$RESPONSE")
    fi
    
    if [ -n "$UPLOAD_URL" ]; then
        echo "✅ Release gefunden/erstellt, lade Asset hoch..."
        
        # Lade ZIP-Datei hoch
        curl -s -X POST \
            -H "Authorization: token $GITHUB_TOKEN" \
            -H "Content-Type: application/zip" \
            --data-binary @"$ZIP_FILE" \
            "$UPLOAD_URL?name=$(basename $ZIP_FILE)"
        
        echo "✅ Asset hochgeladen!"
    else
        echo "❌ Fehler beim Erstellen/Finden des Releases"
        echo "$RESPONSE"
    fi
else
    echo "❌ GitHub CLI oder Token nicht gefunden"
    echo ""
    echo "Bitte manuell auf GitHub erstellen:"
    echo "1. Gehe zu: https://github.com/$REPO/releases/new"
    echo "2. Wähle Tag: $TAG"
    echo "3. Titel: Release v2.2.1"
    echo "4. Beschreibung: Siehe $RELEASE_NOTES"
    echo "5. Lade $ZIP_FILE hoch"
    echo ""
    echo "Oder installiere GitHub CLI:"
    echo "  brew install gh"
    echo "  gh auth login"
    echo "  ./create_release.sh"
fi
