#!/bin/bash

# GitHub Release Script für v2.1.0

REPO="s3vdev/Heizungssteuerung"
TAG="v2.1.0"
ZIP_FILE="ESP32-Heizungssteuerung-v2.1.0.zip"
RELEASE_NOTES="RELEASE_NOTES_v2.1.0.md"

echo "📦 Erstelle GitHub Release für $TAG..."

# Prüfe ob GitHub CLI installiert ist
if command -v gh &> /dev/null; then
    echo "✅ GitHub CLI gefunden"
    
    # Erstelle Release mit gh
    gh release create "$TAG" \
        "$ZIP_FILE" \
        --title "Release v2.1.0" \
        --notes-file "$RELEASE_NOTES" \
        --repo "$REPO"
    
    echo "✅ Release erstellt!"
elif [ -n "$GITHUB_TOKEN" ]; then
    echo "✅ GitHub Token gefunden - erstelle Release via API..."
    
    # Erstelle Release via API
    BODY=$(cat "$RELEASE_NOTES" | sed 's/"/\\"/g' | sed ':a;N;$!ba;s/\n/\\n/g')
    
    RESPONSE=$(curl -s -X POST \
        -H "Authorization: token $GITHUB_TOKEN" \
        -H "Accept: application/vnd.github.v3+json" \
        "https://api.github.com/repos/$REPO/releases" \
        -d "{\"tag_name\":\"$TAG\",\"name\":\"Release v2.1.0\",\"body\":\"$BODY\",\"draft\":false,\"prerelease\":false}")
    
    UPLOAD_URL=$(echo "$RESPONSE" | grep -o '"upload_url":"[^"]*' | cut -d'"' -f4 | sed 's/{?name,label}//')
    
    if [ -n "$UPLOAD_URL" ]; then
        echo "✅ Release erstellt, lade Asset hoch..."
        
        # Lade ZIP-Datei hoch
        curl -s -X POST \
            -H "Authorization: token $GITHUB_TOKEN" \
            -H "Content-Type: application/zip" \
            --data-binary @"$ZIP_FILE" \
            "$UPLOAD_URL?name=$(basename $ZIP_FILE)"
        
        echo "✅ Asset hochgeladen!"
    else
        echo "❌ Fehler beim Erstellen des Releases"
        echo "$RESPONSE"
    fi
else
    echo "❌ GitHub CLI oder Token nicht gefunden"
    echo ""
    echo "Bitte manuell auf GitHub erstellen:"
    echo "1. Gehe zu: https://github.com/$REPO/releases/new"
    echo "2. Wähle Tag: $TAG"
    echo "3. Titel: Release v2.1.0"
    echo "4. Beschreibung: Siehe $RELEASE_NOTES"
    echo "5. Lade $ZIP_FILE hoch"
    echo ""
    echo "Oder installiere GitHub CLI:"
    echo "  brew install gh"
    echo "  gh auth login"
    echo "  ./create_release.sh"
fi
