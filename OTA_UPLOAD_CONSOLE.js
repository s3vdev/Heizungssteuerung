// ========== OTA UPLOAD VIA CONSOLE ==========
// Kopiere diesen Code in die Browser-Konsole (F12) um OTA-Updates durchzuführen
// Auch wenn andere JavaScript-Fehler vorhanden sind

// Funktion zum Hochladen von Firmware
window.uploadFirmwareConsole = function(fileInput) {
    const file = fileInput.files[0];
    if (!file) {
        console.error('Keine Datei ausgewählt!');
        return;
    }
    
    console.log('Starte Firmware-Upload:', file.name, '(' + (file.size / 1024 / 1024).toFixed(2) + ' MB)');
    
    const formData = new FormData();
    formData.append('update', file);
    
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/update', true);
    xhr.setRequestHeader('Authorization', 'Basic ' + btoa('admin:admin'));
    
    xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
            const percent = Math.round((e.loaded / e.total) * 100);
            console.log('Upload: ' + percent + '% (' + (e.loaded / 1024 / 1024).toFixed(1) + ' MB / ' + (e.total / 1024 / 1024).toFixed(1) + ' MB)');
        }
    });
    
    xhr.addEventListener('load', () => {
        if (xhr.status === 200) {
            console.log('✅ Firmware erfolgreich hochgeladen! ESP32 startet neu...');
            console.log('Warte ~20-40 Sekunden, dann sollte der ESP32 wieder erreichbar sein.');
        } else {
            console.error('❌ Fehler:', xhr.status, xhr.responseText);
        }
    });
    
    xhr.addEventListener('error', () => {
        console.error('❌ Netzwerkfehler beim Upload');
    });
    
    xhr.send(formData);
};

// Funktion zum Hochladen von LittleFS (Frontend)
window.uploadFrontendConsole = function(fileInput) {
    const file = fileInput.files[0];
    if (!file) {
        console.error('Keine Datei ausgewählt!');
        return;
    }
    
    console.log('Starte Frontend-Upload:', file.name, '(' + (file.size / 1024).toFixed(1) + ' KB)');
    
    const formData = new FormData();
    formData.append('update', file);
    
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/update-fs', true);
    xhr.setRequestHeader('Authorization', 'Basic ' + btoa('admin:admin'));
    
    xhr.upload.addEventListener('progress', (e) => {
        if (e.lengthComputable) {
            const percent = Math.round((e.loaded / e.total) * 100);
            console.log('Upload: ' + percent + '% (' + (e.loaded / 1024).toFixed(1) + ' KB / ' + (e.total / 1024).toFixed(1) + ' KB)');
        }
    });
    
    xhr.addEventListener('load', () => {
        if (xhr.status === 200) {
            console.log('✅ Frontend erfolgreich hochgeladen! ESP32 startet neu...');
            console.log('Warte ~20-40 Sekunden, dann sollte der ESP32 wieder erreichbar sein.');
        } else {
            console.error('❌ Fehler:', xhr.status, xhr.responseText);
        }
    });
    
    xhr.addEventListener('error', () => {
        console.error('❌ Netzwerkfehler beim Upload');
    });
    
    xhr.send(formData);
};

// Hilfsfunktion: Erstellt ein File-Input Element
window.createOTAInput = function(type) {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.bin';
    input.style.display = 'none';
    document.body.appendChild(input);
    
    input.addEventListener('change', () => {
        if (type === 'firmware') {
            uploadFirmwareConsole(input);
        } else if (type === 'frontend') {
            uploadFrontendConsole(input);
        }
    });
    
    input.click();
    console.log('Datei-Dialog geöffnet. Wähle eine .bin Datei aus.');
    console.log('Typ:', type === 'firmware' ? 'Firmware' : 'Frontend (LittleFS)');
};

console.log('✅ OTA-Upload Funktionen geladen!');
console.log('');
console.log('Verwendung:');
console.log('1. Frontend (LittleFS) hochladen:');
console.log('   createOTAInput("frontend")');
console.log('');
console.log('2. Firmware hochladen:');
console.log('   createOTAInput("firmware")');
console.log('');
console.log('Oder direkt mit einem File-Input Element:');
console.log('   uploadFrontendConsole(document.getElementById("frontendFile"))');
console.log('   uploadFirmwareConsole(document.getElementById("firmwareFile"))');

