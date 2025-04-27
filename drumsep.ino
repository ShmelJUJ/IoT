#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <vector>

#define BUZZER_PIN D1
#define EEPROM_SIZE 4096

ESP8266WebServer server(80);

enum Mode { IDLE, RECORDING, PLAYING };
Mode currentMode = IDLE;

int bpm = 120;
int stepDurationMs = 60000 / bpm / 4;

String currentPreset = "None";

struct DrumHit {
  uint8_t drumCode;
  uint16_t step;
};

std::vector<DrumHit> sequence;
unsigned long recordStart = 0;
unsigned long playStart = 0;
int currentStep = 0;

int lastStep = -1;
unsigned long noteStart = 0;
bool noteActive = false;
uint16_t currentFreq = 0;

void sendHtml() {
  String html = R"rawliteral(
    <!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1"><title>Drum Machine</title>
    <style>
      body { background-color: #0f0f0f; color: #f0f0f0; font-family: 'Segoe UI', sans-serif; text-align: center; padding: 20px; }
      h1 { color: #00ffd5; font-size: 32px; }
      button, select, input {
        background-color: #222; color: #0ff; border: 1px solid #0ff; border-radius: 6px;
        padding: 10px 20px; font-size: 16px; margin: 6px; cursor: pointer; transition: background 0.2s;
      }
      button:hover { background-color: #0ff; color: #000; }
      input::placeholder { color: #888; }
      table { margin: 0 auto; border-collapse: collapse; }
      th, td { border: 1px solid #444; padding: 8px; }
      th { background: #222; }
    </style>
    </head>
    <body>
      <h1>Pseudo Beta-Alpha Version Of Drum Machine :)</h1>
      <p>Status: <span id="status">)rawliteral" + getModeString() + R"rawliteral(</span></p>
      <p>Current Preset: <span id="currentPreset">)rawliteral" + currentPreset + R"rawliteral(</span></p>
      <button onclick="send('kick')">Kick</button>
      <button onclick="send('snare')">Snare</button>
      <button onclick="send('hat')">Hi-Hat</button><br><br>
      <button onclick="send('startRecord')">Record</button>
      <button onclick="send('stopRecord')">Stop Record</button>
      <button onclick="send('playLoop')">Play Loop</button>
      <button onclick="send('stopLoop')">Stop</button>
      <button onclick="send('clear')">Clear</button><br><br>
      <input type="text" id="presetName" placeholder="Preset name">
      <button onclick="savePreset()">Save Preset</button><br><br>
      <select id="presetList"></select>
      <button onclick="loadPreset()">Load & Play</button>
      <h3>Presets:</h3>
      <table id="presetTable"><thead><tr><th>Name</th><th>Action</th></tr></thead><tbody></tbody></table>
      <script>
        function send(cmd) {
          fetch("/action?cmd=" + cmd).then(r => r.text()).then(text => {
            document.getElementById("status").innerText = text;
          });
        }
        function savePreset() {
          const name = document.getElementById("presetName").value;
          fetch("/savePreset?name=" + name).then(() => location.reload());
        }
        function loadPreset() {
          const name = document.getElementById("presetList").value;
          fetch("/loadPreset?name=" + name).then(() => location.reload());
        }
        function deletePreset(name) {
          fetch("/deletePreset?name=" + name).then(() => location.reload());
        }
        fetch("/listPresets")
          .then(r => r.json())
          .then(list => {
            const select = document.getElementById("presetList");
            const table = document.getElementById("presetTable").querySelector("tbody");
            list.forEach(name => {
              let option = document.createElement("option");
              option.text = name;
              select.add(option);
              let row = document.createElement("tr");
              let nameCell = document.createElement("td");
              nameCell.innerText = name;
              let actionCell = document.createElement("td");
              let btn = document.createElement("button");
              btn.innerText = "Delete";
              btn.onclick = () => deletePreset(name);
              actionCell.appendChild(btn);
              row.appendChild(nameCell);
              row.appendChild(actionCell);
              table.appendChild(row);
            });
          });
      </script>
    </body></html>
  )rawliteral";
  server.send(200, "text/html", html);
}

void playSound(uint8_t drumCode) {
  switch (drumCode) {
    case 1: currentFreq = 100; break;
    case 2: currentFreq = 500; break;
    case 3: currentFreq = 1000; break;
    default: currentFreq = 0; break;
  }
  if (currentFreq > 0) {
    tone(BUZZER_PIN, currentFreq);
    noteStart = millis();
    noteActive = true;
  }
}

void savePresetToEEPROM(const String& presetName) {
  EEPROM.begin(EEPROM_SIZE);
  uint8_t count = EEPROM.read(0);
  int addr = 1 + count * 100;
  for (int i = 0; i < 10; i++) {
    EEPROM.write(addr + i, i < presetName.length() ? presetName[i] : 0);
  }
  EEPROM.write(addr + 10, sequence.size());
  for (int i = 0; i < sequence.size(); i++) {
    EEPROM.write(addr + 11 + i * 3, sequence[i].drumCode);
    EEPROM.write(addr + 12 + i * 3, sequence[i].step >> 8);
    EEPROM.write(addr + 13 + i * 3, sequence[i].step & 0xFF);
  }
  count++;
  EEPROM.write(0, count);
  EEPROM.commit();
}

std::vector<String> getPresetNames() {
  EEPROM.begin(EEPROM_SIZE);
  std::vector<String> names;
  int count = EEPROM.read(0);
  for (int i = 0; i < count; i++) {
    String name = "";
    int addr = 1 + i * 100;
    for (int j = 0; j < 10; j++) {
      char c = EEPROM.read(addr + j);
      if (c != 0) name += c;
    }
    names.push_back(name);
  }
  return names;
}

bool loadPresetByName(const String& name) {
  EEPROM.begin(EEPROM_SIZE);
  int count = EEPROM.read(0);
  for (int i = 0; i < count; i++) {
    String storedName = "";
    int addr = 1 + i * 100;
    for (int j = 0; j < 10; j++) {
      char c = EEPROM.read(addr + j);
      if (c != 0) storedName += c;
    }
    if (storedName == name) {
      int size = EEPROM.read(addr + 10);
      sequence.clear();
      for (int k = 0; k < size; k++) {
        uint8_t d = EEPROM.read(addr + 11 + k * 3);
        uint16_t s = (EEPROM.read(addr + 12 + k * 3) << 8) | EEPROM.read(addr + 13 + k * 3);
        sequence.push_back({d, s});
      }
      currentPreset = name;
      return true;
    }
  }
  return false;
}

void deletePresetByName(const String& name) {
  EEPROM.begin(EEPROM_SIZE);
  int count = EEPROM.read(0);
  int newAddr = 1;
  for (int i = 0; i < count; i++) {
    String storedName = "";
    int addr = 1 + i * 100;
    for (int j = 0; j < 10; j++) {
      char c = EEPROM.read(addr + j);
      if (c != 0) storedName += c;
    }
    if (storedName != name) {
      for (int b = 0; b < 100; b++) {
        EEPROM.write(newAddr + b, EEPROM.read(addr + b));
      }
      newAddr += 100;
    }
  }
  int newCount = newAddr / 100;
  EEPROM.write(0, newCount);
  EEPROM.commit();
}

String getModeString() {
  if (currentMode == RECORDING) return "Recording";
  if (currentMode == PLAYING) return "Playing";
  return "Idle";
}

void handleAction() {
  if (!server.hasArg("cmd")) return;
  String cmd = server.arg("cmd");

  if (cmd == "kick" || cmd == "snare" || cmd == "hat") {
    uint8_t drum = (cmd == "kick") ? 1 : (cmd == "snare" ? 2 : 3);
    if (currentMode == RECORDING) {
      unsigned long elapsed = millis() - recordStart;
      uint16_t step = elapsed / stepDurationMs;
      sequence.push_back({drum, step});
    }
    playSound(drum);
  } else if (cmd == "startRecord") {
    sequence.clear();
    currentMode = RECORDING;
    recordStart = millis();
  } else if (cmd == "stopRecord") {
    currentMode = IDLE;
    savePresetToEEPROM("LastSession");
  } else if (cmd == "playLoop") {
    if (!sequence.empty()) {
      currentStep = 0;
      lastStep = -1;
      playStart = millis();
      currentMode = PLAYING;
    }
  } else if (cmd == "stopLoop") {
    currentMode = IDLE;
  } else if (cmd == "clear") {
    sequence.clear();
    currentMode = IDLE;
  }

  server.send(200, "text/plain", getModeString());
}

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  Serial.begin(115200);

  WiFi.softAP("DrumMachineESP", "12345678");
  Serial.println("AP started");
  Serial.println("IP: " + WiFi.softAPIP().toString());

  server.on("/", sendHtml);
  server.on("/action", handleAction);
  server.on("/savePreset", []() {
    String name = server.arg("name");
    savePresetToEEPROM(name);
    server.send(200, "text/plain", "Saved");
  });
  server.on("/loadPreset", []() {
    String name = server.arg("name");
    if (loadPresetByName(name)) {
      currentMode = PLAYING;
      playStart = millis();
      currentStep = 0;
      server.send(200, "text/plain", "Playing");
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
  server.on("/deletePreset", []() {
    String name = server.arg("name");
    deletePresetByName(name);
    server.send(200, "text/plain", "Deleted");
  });
  server.on("/listPresets", []() {
    std::vector<String> names = getPresetNames();
    String json = "[";
    for (int i = 0; i < names.size(); i++) {
      json += "\"" + names[i] + "\"";
      if (i < names.size() - 1) json += ",";
    }
    json += "]";
    server.send(200, "application/json", json);
  });
  server.begin();
}

void loop() {
  server.handleClient();

  if (noteActive && millis() - noteStart > 100) {
    noTone(BUZZER_PIN);
    noteActive = false;
  }

  if (currentMode == PLAYING) {
    unsigned long now = millis() - playStart;
    int step = now / stepDurationMs;

    if (step != lastStep) {
      lastStep = step;
      for (auto& hit : sequence) {
        if (hit.step == step % 64) {
          playSound(hit.drumCode);
        }
      }
    }
  }
}