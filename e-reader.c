#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebServer.h>

// Pins
#define EPD_BUSY  21
#define EPD_CS    10
#define EPD_RST    6
#define EPD_DC     7
#define EPD_SCK   20
#define EPD_MOSI   5
#define EPD_MISO  -1
#define BTN_PIN    3

// Timing
#define IDLE_TIMEOUT_MS   (10UL * 60UL * 1000UL)
#define BTN_LONG_MS        800
#define BTN_DEBOUNCE        50
#define CHARS_PER_PAGE     300

// WiFi
#define WIFI_SSID "Pink-Propeller EB"
#define WIFI_PASS "readbooks"

// Display
#define DISP_W 296
#define DISP_H 128

enum AppState {
  STATE_SPLASH, STATE_MENU, STATE_READING,
  STATE_BOOKLIST, STATE_BOOK_OPTIONS,
  STATE_QUOTES, STATE_DOWNLOAD
};

enum BtnEvent { BTN_NONE, BTN_SHORT, BTN_LONG };

GxEPD2_BW<GxEPD2_290_BS, GxEPD2_290_BS::HEIGHT> display(
  GxEPD2_290_BS(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

AppState currentState = STATE_SPLASH;

const char* menuItems[] = {
  "Continue", "View Booklist", "Saved Quotes/Pages", "Download Books"
};
const int MENU_COUNT = 4;
int menuSelection    = 0;

#define MAX_BOOKS 20
String bookFiles[MAX_BOOKS];
int    bookCount        = 0;
int    bookSelection    = 0;
int    bookOptSelection = 0;

String currentBook = "";
int    currentPage = 0;
int    totalPages  = 0;
String lastBook    = "";
int    lastPage    = 0;

unsigned long lastActivityMs = 0;
bool   wifiActive            = false;
bool   pendingUploadConfirm  = false;
String pendingUploadName     = "";

Preferences prefs;
WebServer server(80);

const char UPLOAD_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Pink Propeller eReader</title>
  <style>
    body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px;background:#fff0f5}
    h1{color:#c0446a;font-size:1.4rem}
    h2{color:#888;font-size:1rem;font-weight:400;margin-top:-12px}
    input[type=file]{margin:12px 0;width:100%}
    button{background:#c0446a;color:#fff;border:none;padding:10px 24px;border-radius:6px;font-size:16px;cursor:pointer}
    button:hover{background:#a03358}
    #msg{margin-top:16px;padding:10px;border-radius:6px;display:none}
    .ok{background:#d4edda;color:#155724}
    .err{background:#f8d7da;color:#721c24}
  </style>
</head>
<body>
  <h1>PINK PROPELLER</h1>
  <h2>like and subscribe</h2>
  <hr>
  <p>Upload a <strong>.txt</strong> book file to the device.</p>
  <form id="frm">
    <input type="file" id="file" accept=".txt" required>
    <br><br>
    <button type="submit">Upload Book</button>
  </form>
  <div id="msg"></div>
  <script>
    document.getElementById('frm').onsubmit = async e => {
      e.preventDefault();
      const f = document.getElementById('file').files[0];
      if (!f) return;
      const fd = new FormData();
      fd.append('file', f, f.name);
      const msg = document.getElementById('msg');
      msg.style.display = 'block';
      msg.className = '';
      msg.textContent = 'Uploading...';
      try {
        const r = await fetch('/upload', {method:'POST', body:fd});
        const t = await r.text();
        msg.className = r.ok ? 'ok' : 'err';
        msg.textContent = t;
      } catch(err) {
        msg.className = 'err';
        msg.textContent = 'Upload failed: ' + err;
      }
    };
  </script>
</body>
</html>
)rawliteral";

// Forward declarations
void drawSplash();
void drawMenuFull();
void drawMenuSelectionOnly();
void drawReadingScreen();
void drawBookList();
void drawBookOptions();
void drawBookOptionsSelectionOnly();
void drawQuotesScreen();
void drawDownloadScreen();
void showUploadConfirmation(const String& fname);
void openBook(const String& fname, int startPage);
void scanBooks();
void initDisplay();
void stopWifi();
void resetIdle();

// NVS helpers
void loadGlobalState() {
  prefs.begin("reader", false);
  lastBook = prefs.getString("lastBook", "");
  lastPage = prefs.getInt("lastPage", 0);
  prefs.end();
}

void saveGlobalState() {
  prefs.begin("reader", false);
  prefs.putString("lastBook", lastBook);
  prefs.putInt("lastPage", lastPage);
  prefs.end();
}

String bookKey(const String& fname) {
  String k = "pg_" + fname;
  if (k.length() > 15) k = k.substring(0, 15);
  return k;
}

int loadBookPage(const String& fname) {
  prefs.begin("bookpages", false);
  int p = prefs.getInt(bookKey(fname).c_str(), 0);
  prefs.end();
  return p;
}

void saveBookPage(const String& fname, int page) {
  prefs.begin("bookpages", false);
  prefs.putInt(bookKey(fname).c_str(), page);
  prefs.end();
}

void deleteBookData(const String& fname) {
  prefs.begin("bookpages", false);
  prefs.remove(bookKey(fname).c_str());
  prefs.end();
  String path = "/" + fname;
  if (SPIFFS.exists(path)) SPIFFS.remove(path);
}

void scanBooks() {
  bookCount = 0;
  File root = SPIFFS.open("/");
  File f    = root.openNextFile();
  while (f && bookCount < MAX_BOOKS) {
    String name = String(f.name());
    if (name.endsWith(".txt")) {
      if (name.startsWith("/")) name = name.substring(1);
      bookFiles[bookCount++] = name;
    }
    f = root.openNextFile();
  }
}

String getBookPage(const String& fname, int pageNum) {
  File f = SPIFFS.open("/" + fname, "r");
  if (!f) return "[File not found]";
  f.seek(pageNum * CHARS_PER_PAGE);
  String buf = "";
  int count  = 0;
  while (f.available() && count < CHARS_PER_PAGE) {
    buf += (char)f.read();
    count++;
  }
  f.close();
  return buf;
}

int getPageCount(const String& fname) {
  File f = SPIFFS.open("/" + fname, "r");
  if (!f) return 0;
  int size = f.size();
  f.close();
  return max(1, (size + CHARS_PER_PAGE - 1) / CHARS_PER_PAGE);
}

// WiFi
void handleUpload() {
  HTTPUpload& upload = server.upload();
  static File uploadFile;
  if (upload.status == UPLOAD_FILE_START) {
    String filename = "/" + upload.filename;
    if (!filename.endsWith(".txt")) filename += ".txt";
    uploadFile = SPIFFS.open(filename, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    scanBooks();
    pendingUploadName    = upload.filename;
    pendingUploadConfirm = true;
    server.send(200, "text/plain",
      "Book uploaded! '" + upload.filename + "' is now in your library.");
  }
}

void startWifi() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", UPLOAD_PAGE);
  });
  server.on("/upload", HTTP_POST,
    []() { server.send(200); },
    handleUpload
  );
  server.begin();
  wifiActive = true;
}

void stopWifi() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiActive = false;
}

void resetIdle() { lastActivityMs = millis(); }
bool isIdle()    { return (millis() - lastActivityMs) > IDLE_TIMEOUT_MS; }

BtnEvent readButton() {
  static bool          lastState   = HIGH;
  static unsigned long pressedAt   = 0;
  static bool          waitRelease = false;

  bool state = digitalRead(BTN_PIN);
  if (lastState == HIGH && state == LOW) {
    pressedAt   = millis();
    waitRelease = true;
  }
  if (waitRelease && state == HIGH) {
    waitRelease       = false;
    unsigned long dur = millis() - pressedAt;
    lastState         = state;
    if (dur > BTN_DEBOUNCE && dur < BTN_LONG_MS) return BTN_SHORT;
    if (dur >= BTN_LONG_MS)                       return BTN_LONG;
  }
  lastState = state;
  return BTN_NONE;
}

void initDisplay() {
  display.init(115200, true, 2, false);
  SPI.end();
  SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);
  display.setRotation(1);
}

void printCentred(const char* text, int16_t y) {
  int16_t  x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((DISP_W - (int16_t)w) / 2 - x1, y);
  display.print(text);
}

int16_t drawWrappedText(const char* text, int16_t x, int16_t y,
                        int16_t maxW, int16_t lineH) {
  String word = "", line = "";
  int16_t cy = y;
  int16_t x1, y1;
  uint16_t w, h;
  for (int i = 0; i <= (int)strlen(text); i++) {
    char c = text[i];
    if (c == ' ' || c == '\n' || c == '\0') {
      String testLine = line + (line.length() ? " " : "") + word;
      display.getTextBounds(testLine.c_str(), x, cy, &x1, &y1, &w, &h);
      if ((int16_t)w > maxW && line.length() > 0) {
        display.setCursor(x, cy);
        display.print(line);
        cy  += lineH;
        line = word;
      } else {
        line = testLine;
      }
      word = "";
      if (c == '\n') {
        display.setCursor(x, cy);
        display.print(line);
        cy  += lineH;
        line = "";
      }
    } else {
      word += c;
    }
  }
  if (line.length()) {
    display.setCursor(x, cy);
    display.print(line);
    cy += lineH;
  }
  return cy;
}

// Screens
void drawSplash() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(1, 1, DISP_W - 2, DISP_H - 2, GxEPD_BLACK);
    display.drawRect(3, 3, DISP_W - 6, DISP_H - 6, GxEPD_BLACK);
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setTextWrap(false);
    printCentred("PINK", 38);
    printCentred("PROPELLER", 60);
    display.drawFastHLine(30, 70, DISP_W - 60, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);
    printCentred("like and subscribe", 95);
  } while (display.nextPage());
  display.hibernate();
}

void drawMenuFull() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(1, 1, DISP_W - 2, DISP_H - 2, GxEPD_BLACK);
    display.fillRect(1, 1, DISP_W - 2, 18, GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(GxEPD_WHITE);
    printCentred("Main Menu", 13);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    for (int i = 0; i < MENU_COUNT; i++) {
      int16_t y = 30 + i * 24;
      if (i == menuSelection) display.fillRect(DISP_W - 14, y, 10, 13, GxEPD_BLACK);
      else                    display.drawRect(DISP_W - 14, y, 10, 13, GxEPD_BLACK);
      display.setCursor(10, y + 10);
      display.print(menuItems[i]);
    }
    display.setFont(nullptr);
    display.setCursor(6, DISP_H - 7);
    display.print("Short=move  Long=select");
  } while (display.nextPage());
}

void drawMenuSelectionOnly() {
  display.setPartialWindow(280, 28, 16, MENU_COUNT * 24);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (int i = 0; i < MENU_COUNT; i++) {
      int16_t y = 30 + i * 24;
      if (i == menuSelection) display.fillRect(DISP_W - 14, y, 10, 13, GxEPD_BLACK);
      else                    display.drawRect(DISP_W - 14, y, 10, 13, GxEPD_BLACK);
    }
  } while (display.nextPage());
}

void drawReadingScreen() {
  String pageText = getBookPage(currentBook, currentPage);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, DISP_W, 15, GxEPD_BLACK);
    display.setFont(nullptr);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(3, 4);
    String hdr = currentBook;
    if (hdr.length() > 24) hdr = hdr.substring(0, 24) + "..";
    display.print(hdr);
    display.setCursor(DISP_W - 46, 4);
    display.printf("p%d/%d", currentPage + 1, totalPages);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    drawWrappedText(pageText.c_str(), 4, 29, DISP_W - 8, 16);
    display.setFont(nullptr);
    display.setCursor(4, DISP_H - 7);
    display.print("Short=next  Long=menu");
  } while (display.nextPage());
}

void drawBookList() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(1, 1, DISP_W - 2, DISP_H - 2, GxEPD_BLACK);
    display.fillRect(1, 1, DISP_W - 2, 15, GxEPD_BLACK);
    display.setFont(nullptr);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 5);
    display.print("Booklist");
    size_t free_ = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    display.setCursor(160, 5);
    display.printf("Free: %dKB", (int)(free_ / 1024));
    display.setTextColor(GxEPD_BLACK);
    if (bookCount == 0) {
      display.setFont(&FreeSans9pt7b);
      display.setCursor(8, 50);
      display.print("No books found.");
      display.setCursor(8, 68);
      display.print("Use Download Books to add.");
    } else {
      int maxVisible = min(bookCount, 5);
      for (int i = 0; i < maxVisible; i++) {
        int16_t y = 20 + i * 20;
        if (i == bookSelection) display.fillRect(DISP_W - 13, y + 2, 9, 11, GxEPD_BLACK);
        else                    display.drawRect(DISP_W - 13, y + 2, 9, 11, GxEPD_BLACK);
        display.setFont(nullptr);
        display.setCursor(4, y + 4);
        String bname = bookFiles[i];
        if (bname.length() > 28) bname = bname.substring(0, 28) + "..";
        display.print(bname);
        display.setCursor(DISP_W - 56, y + 4);
        display.printf("p%d", loadBookPage(bookFiles[i]) + 1);
      }
    }
    display.setFont(nullptr);
    display.setCursor(4, DISP_H - 7);
    display.print("Short=move  Long=options");
  } while (display.nextPage());
}

void drawBookOptions() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(1, 1, DISP_W - 2, DISP_H - 2, GxEPD_BLACK);
    display.fillRect(1, 1, DISP_W - 2, 18, GxEPD_BLACK);
    display.setFont(nullptr);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(6, 6);
    String bname = bookFiles[bookSelection];
    if (bname.length() > 34) bname = bname.substring(0, 34) + "..";
    display.print(bname);
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    if (bookOptSelection == 0) display.fillRect(DISP_W - 14, 40, 10, 13, GxEPD_BLACK);
    else                       display.drawRect(DISP_W - 14, 40, 10, 13, GxEPD_BLACK);
    display.setCursor(10, 50);
    display.print("Open book");
    if (bookOptSelection == 1) display.fillRect(DISP_W - 14, 76, 10, 13, GxEPD_BLACK);
    else                       display.drawRect(DISP_W - 14, 76, 10, 13, GxEPD_BLACK);
    display.setCursor(10, 86);
    display.print("Delete book");
    display.setFont(nullptr);
    display.setCursor(4, DISP_H - 7);
    display.print("Short=move  Long=confirm");
  } while (display.nextPage());
}

void drawBookOptionsSelectionOnly() {
  display.setPartialWindow(280, 34, 16, 60);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    if (bookOptSelection == 0) {
      display.fillRect(DISP_W - 14, 40, 10, 13, GxEPD_BLACK);
      display.drawRect(DISP_W - 14, 76, 10, 13, GxEPD_BLACK);
    } else {
      display.drawRect(DISP_W - 14, 40, 10, 13, GxEPD_BLACK);
      display.fillRect(DISP_W - 14, 76, 10, 13, GxEPD_BLACK);
    }
  } while (display.nextPage());
}

void drawQuotesScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, DISP_W, 15, GxEPD_BLACK);
    display.setFont(nullptr);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 5);
    display.print("Saved Quotes / Pages");
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(8, 42);
    display.print("No saved quotes yet.");
    display.setCursor(8, 60);
    display.print("Long-press while reading");
    display.setCursor(8, 78);
    display.print("to save a page here.");
    display.setFont(nullptr);
    display.setCursor(4, DISP_H - 7);
    display.print("Long=back");
  } while (display.nextPage());
}

void drawDownloadScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, DISP_W, 15, GxEPD_BLACK);
    display.setFont(nullptr);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 5);
    display.print("Download Books - WiFi On");
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, 30);
    display.print("SSID: " WIFI_SSID);
    display.setCursor(4, 48);
    display.print("Pass: " WIFI_PASS);
    display.setCursor(4, 66);
    display.print("URL:  http://192.168.4.1");
    display.setFont(nullptr);
    display.setCursor(4, 86);
    display.print("Open URL in browser to upload .txt");
    display.setCursor(4, DISP_H - 7);
    display.print("Long=stop WiFi & go back");
  } while (display.nextPage());
}

void showUploadConfirmation(const String& fname) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, DISP_W, 15, GxEPD_BLACK);
    display.setFont(nullptr);
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(4, 5);
    display.print("Book Uploaded!");
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(4, 34);
    display.print("Added to library:");
    display.setCursor(4, 52);
    String n = fname;
    if (n.length() > 30) n = n.substring(0, 30) + "..";
    display.print(n);
    display.setCursor(4, 70);
    display.printf("Total books: %d", bookCount);
    display.setCursor(4, 88);
    display.printf("Free space: %dKB", (int)((SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024));
    display.setFont(nullptr);
    display.setCursor(4, DISP_H - 7);
    display.print("Long=back to menu");
  } while (display.nextPage());
}

void openBook(const String& fname, int startPage) {
  currentBook  = fname;
  currentPage  = startPage;
  totalPages   = getPageCount(fname);
  lastBook     = fname;
  lastPage     = startPage;
  saveGlobalState();
  currentState = STATE_READING;
  drawReadingScreen();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(BTN_PIN, INPUT_PULLUP);

  if (!SPIFFS.begin(true)) {
    SPIFFS.format();
    if (!SPIFFS.begin(true)) {
      while (true) delay(1000);
    }
  }

  loadGlobalState();
  scanBooks();
  initDisplay();
  drawSplash();
  currentState = STATE_SPLASH;
  resetIdle();
}

void loop() {
  if (wifiActive && currentState == STATE_DOWNLOAD) {
    server.handleClient();
    if (pendingUploadConfirm) {
      pendingUploadConfirm = false;
      showUploadConfirmation(pendingUploadName);
      resetIdle();
    }
  } else if (wifiActive && currentState != STATE_DOWNLOAD) {
    stopWifi();
  }

  BtnEvent evt = readButton();

  if (isIdle() && currentState != STATE_SPLASH && currentState != STATE_DOWNLOAD) {
    if (currentState == STATE_READING) {
      saveBookPage(currentBook, currentPage);
      saveGlobalState();
    }
    currentState = STATE_SPLASH;
    drawSplash();
    return;
  }

  if (evt == BTN_NONE) return;
  resetIdle();

  if (currentState == STATE_SPLASH) {
    initDisplay();
    menuSelection = 0;
    currentState  = STATE_MENU;
    drawMenuFull();
    return;
  }

  if (currentState == STATE_MENU) {
    if (evt == BTN_SHORT) {
      menuSelection = (menuSelection + 1) % MENU_COUNT;
      drawMenuSelectionOnly();
    } else if (evt == BTN_LONG) {
      switch (menuSelection) {
        case 0:
          if (lastBook.length() > 0)   openBook(lastBook, lastPage);
          else if (bookCount > 0)       openBook(bookFiles[0], 0);
          else { bookSelection = 0; currentState = STATE_BOOKLIST; drawBookList(); }
          break;
        case 1: bookSelection = 0; currentState = STATE_BOOKLIST; drawBookList(); break;
        case 2: currentState = STATE_QUOTES;   drawQuotesScreen();  break;
        case 3: startWifi(); currentState = STATE_DOWNLOAD; drawDownloadScreen(); break;
      }
    }
    return;
  }

  if (currentState == STATE_READING) {
    if (evt == BTN_SHORT) {
      if (currentPage < totalPages - 1) {
        currentPage++;
        lastPage = currentPage;
        saveGlobalState();
        saveBookPage(currentBook, currentPage);
      }
      drawReadingScreen();
    } else if (evt == BTN_LONG) {
      saveBookPage(currentBook, currentPage);
      saveGlobalState();
      currentState = STATE_MENU;
      drawMenuFull();
    }
    return;
  }

  if (currentState == STATE_BOOKLIST) {
    if (evt == BTN_SHORT) {
      if (bookCount > 0) { bookSelection = (bookSelection + 1) % bookCount; drawBookList(); }
      else               { currentState = STATE_MENU; drawMenuFull(); }
    } else if (evt == BTN_LONG) {
      if (bookCount > 0) { bookOptSelection = 0; currentState = STATE_BOOK_OPTIONS; drawBookOptions(); }
      else               { currentState = STATE_MENU; drawMenuFull(); }
    }
    return;
  }

  if (currentState == STATE_BOOK_OPTIONS) {
    if (evt == BTN_SHORT) {
      bookOptSelection = (bookOptSelection + 1) % 2;
      drawBookOptionsSelectionOnly();
    } else if (evt == BTN_LONG) {
      if (bookOptSelection == 0) {
        openBook(bookFiles[bookSelection], loadBookPage(bookFiles[bookSelection]));
      } else {
        String toDelete = bookFiles[bookSelection];
        deleteBookData(toDelete);
        if (lastBook == toDelete) { lastBook = ""; lastPage = 0; saveGlobalState(); }
        scanBooks();
        bookSelection = 0;
        currentState  = STATE_BOOKLIST;
        drawBookList();
      }
    }
    return;
  }

  if (currentState == STATE_QUOTES) {
    if (evt == BTN_LONG) { currentState = STATE_MENU; drawMenuFull(); }
    return;
  }

  if (currentState == STATE_DOWNLOAD) {
    if (evt == BTN_LONG) { stopWifi(); currentState = STATE_MENU; drawMenuFull(); }
    return;
  }
}
