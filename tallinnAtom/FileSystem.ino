// =================== LittleFS / Static Files (statiliste failide teenindamine) ============
String readFileToString(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("[FS] open failed: %s\n", path);
    return "";
  }
  String s;
  while (f.available()) s += (char)f.read();
  f.close();
  return s;
}
// Määrab MIME-tüübi vastavalt faililaiendile
// (vajalik, et brauser kuvaks faile õigesti)
String contentType(const String& path) {
  if (path.endsWith(".html") || path.endsWith(".htm")) return "text/html";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".csv"))  return "text/html";  // CSV kuvatakse HTML-na
  return "text/plain";
}


// Loeb faili LittleFS-ist ja saadab selle kliendile stream'ina
// Tagastab true kui fail leiti ja saadeti
bool streamFromFS(const String& inPath) {

  String path = inPath;

  // Kui küsitakse kataloogi (nt "/") → serveerime index.html
  if (path.endsWith("/"))
    path += "index.html";

  // Kui faili ei ole olemas, katkestame
  if (!LittleFS.exists(path))
    return false;

  // Avame faili lugemiseks
  File f = LittleFS.open(path, "r");
  if (!f)
    return false;

  // Edastame faili brauserile õige MIME-tüübiga
  server.streamFile(f, contentType(path));
  f.close();
  return true;
}


// Töötleb kõik staatiliste failide päringud
// Access rules:
//  • AP režiim – kõik failid lubatud
//  • Client režiim – lubatud AINULT "/" ja "/index.html"
void handleFileRequest() {

  const String path = server.uri();

  // Kliendirežiimis piirame ligipääsu failidele
  if (isClientMode()) {
    // Lubatud ainult: "/" ja "/index.html"
    if (!(path == "/" || path == "/index.html")) {
      server.send(403, "text/plain", "Forbidden in client mode");
      return;
    }
  }

  // Proovime teenindada faili
  if (!streamFromFS(path)) {
    // Faili pole olemas → 404
    server.send(404, "text/plain", "Not found");
  }
}
