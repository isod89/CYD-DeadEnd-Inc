#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#include "CYD28_TouchscreenR.h"

namespace deadend {

constexpr uint8_t kBacklightPin = 21;
constexpr bool kBacklightActiveHigh = true;
constexpr uint8_t kScreenRotation = 1;
constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;
constexpr uint8_t kSdCsPin = 5;
constexpr uint8_t kSdSckPin = 18;
constexpr uint8_t kSdMisoPin = 19;
constexpr uint8_t kSdMosiPin = 23;

constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kPanel = 0x10A2;
constexpr uint16_t kPanelAlt = 0x18C3;
constexpr uint16_t kText = 0xDEDB;
constexpr uint16_t kMuted = 0x7BEF;
constexpr uint16_t kAccent = 0x54F7;
constexpr uint16_t kAccent2 = 0x7DFF;
constexpr uint16_t kGood = 0x3FEA;
constexpr uint16_t kWarn = 0xF5E0;
constexpr uint16_t kBad = 0xE104;

constexpr uint32_t kTouchDebounceMs = 160;
constexpr uint32_t kAutoSaveMs = 30000;
constexpr uint32_t kAutoClockTickMs = 25000;
constexpr int kAutoClockStepMinutes = 5;
constexpr int kShiftLengthMinutes = 360;
constexpr int kPassiveHeatCooldownMinutes = 120;
constexpr int kMaxLogs = 12;
constexpr int kMaxEvents = 256;
constexpr int kMaxDistricts = 64;
constexpr int kMaxContacts = 96;
constexpr int kMaxProducts = 24;
constexpr int kMaxCampaignStages = 16;
constexpr int kActionCount = 6;
constexpr int kVisibleCardCount = 4;

enum class Page : uint8_t { Home, Ops, Map, Contacts, Log };
enum class ActionId : uint8_t {
  None, PrevProduct, NextProduct, BuyPrecursors, Cook, Pack, Deliver, Bribe,
  Recruit, UpgradeLab, Meet, Ask, PayDebt, CoolHeat, EndTurn, SaveGame, LoadGame, ShowMission
};

struct Button { int16_t x; int16_t y; int16_t w; int16_t h; const char* label; };
struct ProductDef { String id; String label; String desc; int basePrice=20; int risk=8; int chemCost=2; int batchYield=2; int heatGain=4; int requiredLab=1; int requiredRep=0; };
struct DistrictDef { String id; String label; String desc; int risk=8; int unlockRep=0; int demand[kMaxProducts]={6,6,6}; int control=0; int saturation=0; };
struct ContactDef { String id; String label; String role; String desc; int district=0; int loyalty=45; int volatility=20; int preferredProduct=0; };
struct EventDef { String id; String text; int minDay=1; int minHeat=0; int minRep=0; int cashDelta=0; int heatDelta=0; int repDelta=0; int debtDelta=0; bool fired=false; };
struct CampaignStage {
  String id;
  String title;
  String objective;
  String story;
  String unlockDistrictId;
  int minDay=1;
  int minRep=0;
  int minCash=0;
  int maxHeat=100;
  int maxDebt=9999;
  int minControl=0;
  int rewardCash=0;
  int rewardRep=0;
  int rewardLab=0;
  int rewardCrew=0;
  int rewardStorage=0;
  bool completed=false;
};
struct ActionSlot { ActionId id = ActionId::None; String label; };
struct GameState {
  bool sdOk=false; bool contentOk=false; bool dirty=true; bool saveDirty=true;
  String title="Dead End Inc."; String tagline="Build quiet. Stay alive."; String tip="Keep heat low. Spread routes. Protect cash.";
  Page page=Page::Home; uint8_t selectedProduct=0; uint8_t selectedDistrict=0; uint8_t selectedContact=0;
  uint8_t districtPage=0; uint8_t contactPage=0;
  bool showMission=false;
  uint8_t ambientFrame=0;
  uint8_t campaignStage=0; bool campaignWon=false;
  int day=1; int phase=0; int cash=180; int debt=120; int heat=10; int rep=5; int crew=1; int precursor=6; int lab=1; int security=1; int storage=1;
  int phaseMinute=0;
  int heatCooldownMinutes=0;
  int cooked[kMaxProducts]={0}; int packed[kMaxProducts]={0};
  String banner="First shift. Keep it tidy."; String logs[kMaxLogs]; int logCount=0; uint32_t lastTouchMs=0; uint32_t lastSaveMs=0;
} state;

TFT_eSPI tft;
CYD28_TouchR touch(kScreenWidth, kScreenHeight);
SPIClass sdSpi(VSPI);
ProductDef gProducts[kMaxProducts];
DistrictDef gDistricts[kMaxDistricts];
ContactDef gContacts[kMaxContacts];
EventDef gEvents[kMaxEvents];
CampaignStage gCampaign[kMaxCampaignStages];
ActionSlot gActions[kActionCount];
int gProductCount = 0;
int gDistrictCount = 0;
int gContactCount = 0;
int gEventCount = 0;
int gCampaignCount = 0;

void stabilizeStats();
void validateContent();
void clampSelections();
void evaluateCampaignProgress();
String repObjective();
void drawAmbientLayer(bool force = false);
void applyPassiveHeatCooldown(int minutes);
void advanceWorldTime(int minutes);
void endTurn();

const Button kTabs[] = {
  {6, 212, 58, 22, "HOME"}, {68, 212, 58, 22, "OPS"}, {130, 212, 58, 22, "MAP"},
  {192, 212, 58, 22, "CREW"}, {254, 212, 60, 22, "LOG"},
};
const Button kActionButtons[kActionCount] = {
  {8, 164, 98, 21, ""}, {110, 164, 98, 21, ""}, {212, 164, 98, 21, ""},
  {8, 187, 98, 21, ""}, {110, 187, 98, 21, ""}, {212, 187, 98, 21, ""},
};
const char* kPhaseNames[] = {"Morning", "Evening", "Late"};
const int kPhaseStartHours[] = {8, 14, 20};

bool within(int16_t px, int16_t py, const Button& b) { return px >= b.x && py >= b.y && px < (b.x + b.w) && py < (b.y + b.h); }
bool within(int16_t px, int16_t py, int16_t x, int16_t y, int16_t w, int16_t h) { return px >= x && py >= y && px < (x + w) && py < (y + h); }
int clampStat(int value, int minValue, int maxValue) { if (value < minValue) return minValue; if (value > maxValue) return maxValue; return value; }
String trimLine(String s) { s.trim(); return s; }

String lineField(const String& line, int index) {
  int start = 0;
  for (int current = 0; current < index; ++current) {
    int sep = line.indexOf('|', start);
    if (sep < 0) return "";
    start = sep + 1;
  }
  int end = line.indexOf('|', start);
  String out = end < 0 ? line.substring(start) : line.substring(start, end);
  out.trim();
  return out;
}

int fieldCount(const String& line) {
  if (line.isEmpty()) return 0;
  int count = 1;
  for (int i = 0; i < line.length(); ++i) {
    if (line.charAt(i) == '|') count++;
  }
  return count;
}

int findProductIndexById(const String& id) {
  for (int i = 0; i < gProductCount; ++i) if (gProducts[i].id == id) return i;
  return 0;
}

int findDistrictIndexById(const String& id) {
  for (int i = 0; i < gDistrictCount; ++i) if (gDistricts[i].id == id) return i;
  return -1;
}

int findContactIndexById(const String& id) {
  for (int i = 0; i < gContactCount; ++i) if (gContacts[i].id == id) return i;
  return -1;
}

int findEventIndexById(const String& id) {
  for (int i = 0; i < gEventCount; ++i) if (gEvents[i].id == id) return i;
  return -1;
}

int defaultRequiredLabFor(const ProductDef& p) {
  if (p.risk >= 21 || p.basePrice >= 95) return 4;
  if (p.risk >= 17 || p.basePrice >= 75) return 3;
  if (p.risk >= 12 || p.basePrice >= 50) return 2;
  return 1;
}

int defaultRequiredRepFor(const ProductDef& p) {
  if (p.risk >= 21 || p.basePrice >= 100) return 34;
  if (p.risk >= 18 || p.basePrice >= 85) return 24;
  if (p.risk >= 15 || p.basePrice >= 65) return 16;
  if (p.risk >= 11 || p.basePrice >= 40) return 8;
  return 0;
}

void pushLog(const String& text) {
  if (text.isEmpty()) return;
  for (int i = kMaxLogs - 1; i > 0; --i) state.logs[i] = state.logs[i - 1];
  state.logs[0] = text;
  if (state.logCount < kMaxLogs) state.logCount++;
  state.banner = text;
  state.dirty = true;
  state.saveDirty = true;
}

String savePath() { return "/deadend/saves/slot1.cfg"; }
String historyPath() { return "/deadend/saves/history.log"; }

int pageCountFor(int total) {
  if (total <= 0) return 1;
  return (total + kVisibleCardCount - 1) / kVisibleCardCount;
}

int currentClockHour() {
  return (kPhaseStartHours[state.phase] + (state.phaseMinute / 60)) % 24;
}

int currentClockMinute() {
  return state.phaseMinute % 60;
}

String currentClockText() {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", currentClockHour(), currentClockMinute());
  return String(buf);
}

void appendHistory(const String& text) {
  if (!state.sdOk) return;
  File f = SD.open(historyPath(), FILE_APPEND);
  if (!f) return;
  f.printf("D%02d-%s | %s\n", state.day, kPhaseNames[state.phase], text.c_str());
  f.close();
}

void setBacklight(bool on) {
  pinMode(kBacklightPin, OUTPUT);
  digitalWrite(kBacklightPin, (on == kBacklightActiveHigh) ? HIGH : LOW);
}

void ensureDir(const char* path) {
  if (state.sdOk && !SD.exists(path)) SD.mkdir(path);
}

void setDefaultProducts() {
  gProductCount = 3;
  gProducts[0].id = "glow"; gProducts[0].label = "Glow"; gProducts[0].desc = "Cheap shimmer for club crowds."; gProducts[0].basePrice = 22; gProducts[0].risk = 8; gProducts[0].chemCost = 2; gProducts[0].batchYield = 2; gProducts[0].heatGain = 4; gProducts[0].requiredLab = 1; gProducts[0].requiredRep = 0;
  gProducts[1].id = "dust"; gProducts[1].label = "Dust"; gProducts[1].desc = "Reliable street seller with steady demand."; gProducts[1].basePrice = 34; gProducts[1].risk = 11; gProducts[1].chemCost = 3; gProducts[1].batchYield = 3; gProducts[1].heatGain = 5; gProducts[1].requiredLab = 1; gProducts[1].requiredRep = 6;
  gProducts[2].id = "hex"; gProducts[2].label = "Hex"; gProducts[2].desc = "Premium rush. Big money. Big eyes on it."; gProducts[2].basePrice = 52; gProducts[2].risk = 15; gProducts[2].chemCost = 4; gProducts[2].batchYield = 5; gProducts[2].heatGain = 7; gProducts[2].requiredLab = 2; gProducts[2].requiredRep = 14;
}

void setDefaultDistricts() {
  gDistrictCount = 4;
  gDistricts[0].id = "motel"; gDistricts[0].label = "Motel Strip"; gDistricts[0].desc = "Cheap rooms and desperate buyers."; gDistricts[0].risk = 8; gDistricts[0].unlockRep = 0; gDistricts[0].demand[0] = 7; gDistricts[0].demand[1] = 5; gDistricts[0].demand[2] = 2; gDistricts[0].control = 0; gDistricts[0].saturation = 0;
  gDistricts[1].id = "east"; gDistricts[1].label = "East Block"; gDistricts[1].desc = "Busy corners with thin patience."; gDistricts[1].risk = 12; gDistricts[1].unlockRep = 6; gDistricts[1].demand[0] = 4; gDistricts[1].demand[1] = 8; gDistricts[1].demand[2] = 5; gDistricts[1].control = 0; gDistricts[1].saturation = 0;
  gDistricts[2].id = "station"; gDistricts[2].label = "Station Market"; gDistricts[2].desc = "Fast cash and faster cameras."; gDistricts[2].risk = 15; gDistricts[2].unlockRep = 12; gDistricts[2].demand[0] = 5; gDistricts[2].demand[1] = 6; gDistricts[2].demand[2] = 7; gDistricts[2].control = 0; gDistricts[2].saturation = 0;
  gDistricts[3].id = "club"; gDistricts[3].label = "Club Row"; gDistricts[3].desc = "Premium buyers and premium trouble."; gDistricts[3].risk = 18; gDistricts[3].unlockRep = 20; gDistricts[3].demand[0] = 8; gDistricts[3].demand[1] = 4; gDistricts[3].demand[2] = 10; gDistricts[3].control = 0; gDistricts[3].saturation = 0;
}

void setDefaultContacts() {
  gContactCount = 5;
  gContacts[0].id = "june"; gContacts[0].label = "June Mercer"; gContacts[0].role = "supplier"; gContacts[0].desc = "Old chem broker with a calm voice."; gContacts[0].district = 0; gContacts[0].loyalty = 52; gContacts[0].volatility = 16; gContacts[0].preferredProduct = 0;
  gContacts[1].id = "rico"; gContacts[1].label = "Rico Vale"; gContacts[1].role = "runner"; gContacts[1].desc = "Fast feet, loose mouth."; gContacts[1].district = 1; gContacts[1].loyalty = 45; gContacts[1].volatility = 28; gContacts[1].preferredProduct = 1;
  gContacts[2].id = "mara"; gContacts[2].label = "Mara Quill"; gContacts[2].role = "fixer"; gContacts[2].desc = "Can cool heat for a price."; gContacts[2].district = 2; gContacts[2].loyalty = 58; gContacts[2].volatility = 14; gContacts[2].preferredProduct = 2;
  gContacts[3].id = "sol"; gContacts[3].label = "Sol Pike"; gContacts[3].role = "client"; gContacts[3].desc = "Pays big, disappears bigger."; gContacts[3].district = 3; gContacts[3].loyalty = 40; gContacts[3].volatility = 22; gContacts[3].preferredProduct = 2;
  gContacts[4].id = "nix"; gContacts[4].label = "Nix Ortega"; gContacts[4].role = "scout"; gContacts[4].desc = "Always knows who is watching."; gContacts[4].district = 1; gContacts[4].loyalty = 47; gContacts[4].volatility = 20; gContacts[4].preferredProduct = 1;
}

void setDefaultEvents() {
  gEventCount = 6;
  gEvents[0].id = "late_fee"; gEvents[0].text = "The collector doubled your late fee overnight."; gEvents[0].minDay = 2; gEvents[0].minHeat = 0; gEvents[0].minRep = 0; gEvents[0].cashDelta = -20; gEvents[0].heatDelta = 0; gEvents[0].repDelta = -2; gEvents[0].debtDelta = 12; gEvents[0].fired = false;
  gEvents[1].id = "camera_sweep"; gEvents[1].text = "New street cameras went up across your route."; gEvents[1].minDay = 3; gEvents[1].minHeat = 18; gEvents[1].minRep = 0; gEvents[1].cashDelta = 0; gEvents[1].heatDelta = 8; gEvents[1].repDelta = 0; gEvents[1].debtDelta = 0; gEvents[1].fired = false;
  gEvents[2].id = "club_invite"; gEvents[2].text = "A club promoter wants a premium drop tonight."; gEvents[2].minDay = 4; gEvents[2].minHeat = 0; gEvents[2].minRep = 12; gEvents[2].cashDelta = 35; gEvents[2].heatDelta = 4; gEvents[2].repDelta = 3; gEvents[2].debtDelta = 0; gEvents[2].fired = false;
  gEvents[3].id = "runner_cut"; gEvents[3].text = "A runner skimmed a box off the top."; gEvents[3].minDay = 5; gEvents[3].minHeat = 12; gEvents[3].minRep = 0; gEvents[3].cashDelta = -18; gEvents[3].heatDelta = 5; gEvents[3].repDelta = -1; gEvents[3].debtDelta = 0; gEvents[3].fired = false;
  gEvents[4].id = "quiet_window"; gEvents[4].text = "A rare quiet window opens across the district."; gEvents[4].minDay = 6; gEvents[4].minHeat = 0; gEvents[4].minRep = 8; gEvents[4].cashDelta = 0; gEvents[4].heatDelta = -6; gEvents[4].repDelta = 2; gEvents[4].debtDelta = 0; gEvents[4].fired = false;
  gEvents[5].id = "supplier_credit"; gEvents[5].text = "June floats you extra precursor on trust."; gEvents[5].minDay = 7; gEvents[5].minHeat = 0; gEvents[5].minRep = 5; gEvents[5].cashDelta = 0; gEvents[5].heatDelta = 0; gEvents[5].repDelta = 1; gEvents[5].debtDelta = -10; gEvents[5].fired = false;
}

void upsertProduct(const ProductDef& p) {
  if (p.id.isEmpty() || p.label.isEmpty()) return;
  int idx = findProductIndexById(p.id);
  if (idx >= 0 && idx < gProductCount && gProducts[idx].id == p.id) {
    gProducts[idx] = p;
    return;
  }
  if (gProductCount < kMaxProducts) {
    gProducts[gProductCount++] = p;
  }
}

void upsertDistrict(const DistrictDef& d) {
  if (d.id.isEmpty() || d.label.isEmpty()) return;
  int idx = findDistrictIndexById(d.id);
  if (idx >= 0) {
    int control = gDistricts[idx].control;
    int saturation = gDistricts[idx].saturation;
    gDistricts[idx] = d;
    gDistricts[idx].control = control;
    gDistricts[idx].saturation = saturation;
    return;
  }
  if (gDistrictCount < kMaxDistricts) {
    gDistricts[gDistrictCount++] = d;
  }
}

void upsertContact(const ContactDef& c) {
  if (c.id.isEmpty() || c.label.isEmpty()) return;
  int idx = findContactIndexById(c.id);
  if (idx >= 0) {
    int loyalty = gContacts[idx].loyalty;
    gContacts[idx] = c;
    gContacts[idx].loyalty = loyalty;
    return;
  }
  if (gContactCount < kMaxContacts) {
    gContacts[gContactCount++] = c;
  }
}

void upsertEvent(const EventDef& e) {
  if (e.id.isEmpty() || e.text.isEmpty()) return;
  int idx = findEventIndexById(e.id);
  if (idx >= 0) {
    bool fired = gEvents[idx].fired;
    gEvents[idx] = e;
    gEvents[idx].fired = fired;
    return;
  }
  if (gEventCount < kMaxEvents) {
    gEvents[gEventCount++] = e;
  }
}

void loadProductsStream(File& f) {
  while (f.available()) {
    String line = trimLine(f.readStringUntil('\n'));
    if (line.isEmpty() || line.startsWith("#")) continue;
    ProductDef p;
    int totalFields = fieldCount(line);
    p.id = lineField(line, 0);
    p.label = lineField(line, 1);
    p.basePrice = lineField(line, 2).toInt();
    p.risk = lineField(line, 3).toInt();
    p.chemCost = lineField(line, 4).toInt();
    p.batchYield = lineField(line, 5).toInt();
    p.heatGain = lineField(line, 6).toInt();
    if (totalFields >= 10) {
      p.requiredLab = max(1, static_cast<int>(lineField(line, 7).toInt()));
      p.requiredRep = max(0, static_cast<int>(lineField(line, 8).toInt()));
      p.desc = lineField(line, 9);
    } else {
      p.requiredLab = defaultRequiredLabFor(p);
      p.requiredRep = defaultRequiredRepFor(p);
      p.desc = lineField(line, 7);
    }
    upsertProduct(p);
  }
}

void loadDistrictsStream(File& f) {
  while (f.available()) {
    String line = trimLine(f.readStringUntil('\n'));
    if (line.isEmpty() || line.startsWith("#")) continue;
    DistrictDef d;
    for (int i = 0; i < kMaxProducts; ++i) d.demand[i] = 5;
    d.id = lineField(line, 0);
    d.label = lineField(line, 1);
    d.risk = lineField(line, 2).toInt();
    d.unlockRep = lineField(line, 3).toInt();
    int totalFields = fieldCount(line);
    int descIndex = max(7, totalFields - 1);
    int demandSlots = min(kMaxProducts, max(0, descIndex - 4));
    for (int i = 0; i < demandSlots; ++i) {
      String demandValue = lineField(line, 4 + i);
      if (demandValue.length() > 0) d.demand[i] = demandValue.toInt();
    }
    d.desc = lineField(line, descIndex);
    upsertDistrict(d);
  }
}

void loadContactsStream(File& f) {
  while (f.available()) {
    String line = trimLine(f.readStringUntil('\n'));
    if (line.isEmpty() || line.startsWith("#")) continue;
    ContactDef c;
    c.id = lineField(line, 0);
    c.label = lineField(line, 1);
    c.role = lineField(line, 2);
    c.district = max(0, findDistrictIndexById(lineField(line, 3)));
    c.loyalty = lineField(line, 4).toInt();
    c.volatility = lineField(line, 5).toInt();
    c.preferredProduct = findProductIndexById(lineField(line, 6));
    c.desc = lineField(line, 7);
    upsertContact(c);
  }
}

void loadEventsStream(File& f) {
  while (f.available()) {
    String line = trimLine(f.readStringUntil('\n'));
    if (line.isEmpty() || line.startsWith("#")) continue;
    EventDef e;
    e.id = lineField(line, 0);
    e.minDay = lineField(line, 1).toInt();
    e.minHeat = lineField(line, 2).toInt();
    e.minRep = lineField(line, 3).toInt();
    e.text = lineField(line, 4);
    e.cashDelta = lineField(line, 5).toInt();
    e.heatDelta = lineField(line, 6).toInt();
    e.repDelta = lineField(line, 7).toInt();
    e.debtDelta = lineField(line, 8).toInt();
    upsertEvent(e);
  }
}

void loadDirectoryFiles(const char* path, void (*loader)(File&)) {
  if (!state.sdOk || !SD.exists(path)) return;
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return;
  }
  while (true) {
    File file = dir.openNextFile();
    if (!file) break;
    if (!file.isDirectory()) {
      loader(file);
    }
    file.close();
  }
  dir.close();
}

void setDefaultCampaign() {
  gCampaignCount = 10;

  gCampaign[0].id = "stage_01"; gCampaign[0].title = "Open Tabs"; gCampaign[0].objective = "Reach $260, keep heat under 40, survive day 2."; gCampaign[0].story = "The room is cheap, the debt is not, and nobody expects you to last."; gCampaign[0].minDay = 2; gCampaign[0].minCash = 260; gCampaign[0].maxHeat = 40; gCampaign[0].maxDebt = 260; gCampaign[0].rewardCash = 20; gCampaign[0].rewardRep = 2;
  gCampaign[1].id = "stage_02"; gCampaign[1].title = "Claim East"; gCampaign[1].objective = "Reach rep 8 and hold one district at control 2."; gCampaign[1].story = "The first real corner does not look like much until it starts paying for itself."; gCampaign[1].minRep = 8; gCampaign[1].minControl = 2; gCampaign[1].rewardCash = 25; gCampaign[1].rewardRep = 2; gCampaign[1].unlockDistrictId = "east";
  gCampaign[2].id = "stage_03"; gCampaign[2].title = "Reliable Flow"; gCampaign[2].objective = "Reach $360 with heat under 46 and debt under 240."; gCampaign[2].story = "Routine is where small crews stop improvising and start looking dangerous."; gCampaign[2].minCash = 360; gCampaign[2].maxHeat = 46; gCampaign[2].maxDebt = 240; gCampaign[2].rewardLab = 1; gCampaign[2].rewardCash = 30;
  gCampaign[3].id = "stage_04"; gCampaign[3].title = "Market Entry"; gCampaign[3].objective = "Reach rep 16 and survive until day 5."; gCampaign[3].story = "Station Market notices you the moment you stop looking temporary."; gCampaign[3].minDay = 5; gCampaign[3].minRep = 16; gCampaign[3].rewardCash = 30; gCampaign[3].rewardRep = 3; gCampaign[3].unlockDistrictId = "station";
  gCampaign[4].id = "stage_05"; gCampaign[4].title = "Tight Network"; gCampaign[4].objective = "Reach $520 and keep one district at control 6."; gCampaign[4].story = "Everyone starts calling it a network the moment enough people can betray it."; gCampaign[4].minCash = 520; gCampaign[4].minControl = 6; gCampaign[4].rewardCrew = 1; gCampaign[4].rewardRep = 2;
  gCampaign[5].id = "stage_06"; gCampaign[5].title = "Clean Hands"; gCampaign[5].objective = "Reach rep 24 while keeping heat under 44."; gCampaign[5].story = "The city rewards discipline just long enough to make you trust it."; gCampaign[5].minRep = 24; gCampaign[5].maxHeat = 44; gCampaign[5].rewardStorage = 4; gCampaign[5].rewardCash = 35;
  gCampaign[6].id = "stage_07"; gCampaign[6].title = "Club Whisper"; gCampaign[6].objective = "Reach rep 32, cash 760, and debt under 180."; gCampaign[6].story = "By the time the clubs know your name, somebody richer already wants a cut."; gCampaign[6].minRep = 32; gCampaign[6].minCash = 760; gCampaign[6].maxDebt = 180; gCampaign[6].rewardCash = 40; gCampaign[6].rewardRep = 3; gCampaign[6].unlockDistrictId = "club";
  gCampaign[7].id = "stage_08"; gCampaign[7].title = "Pressure Ladder"; gCampaign[7].objective = "Reach control 10 and survive until day 10."; gCampaign[7].story = "Bigger routes make small mistakes visible from much farther away."; gCampaign[7].minDay = 10; gCampaign[7].minControl = 10; gCampaign[7].rewardLab = 1; gCampaign[7].rewardCrew = 1;
  gCampaign[8].id = "stage_09"; gCampaign[8].title = "Shadow Brand"; gCampaign[8].objective = "Reach rep 45, cash 1100, debt under 120."; gCampaign[8].story = "Now the city talks about you even when your crew is not in the room."; gCampaign[8].minRep = 45; gCampaign[8].minCash = 1100; gCampaign[8].maxDebt = 120; gCampaign[8].rewardCash = 80; gCampaign[8].rewardRep = 4;
  gCampaign[9].id = "stage_10"; gCampaign[9].title = "Top Floor"; gCampaign[9].objective = "Reach rep 60, control 16, cash 1500, and keep heat under 68."; gCampaign[9].story = "You are not building a crew anymore. You are building weather."; gCampaign[9].minRep = 60; gCampaign[9].minCash = 1500; gCampaign[9].maxHeat = 68; gCampaign[9].minControl = 16; gCampaign[9].rewardCash = 120; gCampaign[9].rewardRep = 5;
}

void upsertCampaignStage(const CampaignStage& s) {
  if (s.id.isEmpty() || s.title.isEmpty() || s.objective.isEmpty()) return;
  for (int i = 0; i < gCampaignCount; ++i) {
    if (gCampaign[i].id == s.id) {
      bool completed = gCampaign[i].completed;
      gCampaign[i] = s;
      gCampaign[i].completed = completed;
      return;
    }
  }
  if (gCampaignCount < kMaxCampaignStages) {
    gCampaign[gCampaignCount++] = s;
  }
}

void loadCampaignStream(File& f) {
  while (f.available()) {
    String line = trimLine(f.readStringUntil('\n'));
    if (line.isEmpty() || line.startsWith("#")) continue;
    CampaignStage s;
    s.id = lineField(line, 0);
    s.title = lineField(line, 1);
    s.objective = lineField(line, 2);
    s.minDay = lineField(line, 3).toInt();
    s.minRep = lineField(line, 4).toInt();
    s.minCash = lineField(line, 5).toInt();
    s.maxHeat = lineField(line, 6).toInt();
    s.maxDebt = lineField(line, 7).toInt();
    s.minControl = lineField(line, 8).toInt();
    s.rewardCash = lineField(line, 9).toInt();
    s.rewardRep = lineField(line, 10).toInt();
    s.rewardLab = lineField(line, 11).toInt();
    s.rewardCrew = lineField(line, 12).toInt();
    s.rewardStorage = lineField(line, 13).toInt();
    s.unlockDistrictId = lineField(line, 14);
    s.story = lineField(line, 15);
    upsertCampaignStage(s);
  }
}

void loadCampaignFile() {
  if (!state.sdOk) return;
  File f = SD.open("/deadend/campaign/campaign.txt");
  if (f) {
    loadCampaignStream(f);
    f.close();
  }
  loadDirectoryFiles("/deadend/mods/campaign", loadCampaignStream);
}

int maxDistrictControl() {
  int best = 0;
  for (int i = 0; i < gDistrictCount; ++i) {
    if (gDistricts[i].control > best) best = gDistricts[i].control;
  }
  return best;
}

String currentObjective() {
  if (state.campaignWon) return "You own the room. Stay there.";
  if (state.campaignStage < gCampaignCount) return gCampaign[state.campaignStage].objective;
  return repObjective();
}

String currentChapterTitle() {
  if (state.campaignWon) return "TOP FLOOR";
  if (state.campaignStage < gCampaignCount) return gCampaign[state.campaignStage].title;
  return "FREE PLAY";
}
void loadConfigFile() {
  if (!state.sdOk) return;
  File f = SD.open("/deadend/world/config.cfg");
  if (!f) return;
  while (f.available()) {
    String line = trimLine(f.readStringUntil('\n'));
    if (line.isEmpty() || line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = trimLine(line.substring(0, eq));
    String value = trimLine(line.substring(eq + 1));
    if (key == "title") state.title = value;
    else if (key == "tagline") state.tagline = value;
    else if (key == "tip") state.tip = value;
    else if (key == "start_cash") state.cash = value.toInt();
    else if (key == "start_debt") state.debt = value.toInt();
    else if (key == "start_precursor") state.precursor = value.toInt();
  }
  f.close();
}

void loadProductsFile() {
  if (!state.sdOk) return;
  File f = SD.open("/deadend/products/products.txt");
  if (!f) return;
  loadProductsStream(f);
  f.close();
  loadDirectoryFiles("/deadend/mods/products", loadProductsStream);
}

void loadDistrictsFile() {
  if (!state.sdOk) return;
  File f = SD.open("/deadend/world/districts.txt");
  if (!f) return;
  loadDistrictsStream(f);
  f.close();
  loadDirectoryFiles("/deadend/mods/districts", loadDistrictsStream);
}

void loadContactsFile() {
  if (!state.sdOk) return;
  File f = SD.open("/deadend/characters/contacts.txt");
  if (!f) return;
  loadContactsStream(f);
  f.close();
  loadDirectoryFiles("/deadend/mods/characters", loadContactsStream);
}

void loadEventsFile() {
  if (!state.sdOk) return;
  File f = SD.open("/deadend/events/events.txt");
  if (!f) return;
  loadEventsStream(f);
  f.close();
  loadDirectoryFiles("/deadend/mods/events", loadEventsStream);
}

void saveState() {
  if (!state.sdOk) return;
  ensureDir("/deadend");
  ensureDir("/deadend/saves");
  SD.remove(savePath());
  File f = SD.open(savePath(), FILE_WRITE);
  if (!f) return;
  f.printf("day=%d\nphase=%d\nphase_minute=%d\nheat_cooldown=%d\ncash=%d\ndebt=%d\nheat=%d\nrep=%d\ncrew=%d\nprecursor=%d\nlab=%d\nsecurity=%d\nstorage=%d\nselected_product=%d\nselected_district=%d\nselected_contact=%d\n",
      state.day, state.phase, state.phaseMinute, state.heatCooldownMinutes, state.cash, state.debt, state.heat, state.rep, state.crew, state.precursor,
      state.lab, state.security, state.storage, state.selectedProduct, state.selectedDistrict, state.selectedContact);
  f.printf("campaign_stage=%d\ncampaign_won=%d\n", state.campaignStage, state.campaignWon ? 1 : 0);
  for (int i = 0; i < gProductCount; ++i) f.printf("cooked_%d=%d\npacked_%d=%d\n", i, state.cooked[i], i, state.packed[i]);
  for (int i = 0; i < gDistrictCount; ++i) f.printf("district_control_%d=%d\ndistrict_sat_%d=%d\n", i, gDistricts[i].control, i, gDistricts[i].saturation);
  for (int i = 0; i < gContactCount; ++i) f.printf("contact_loyal_%d=%d\n", i, gContacts[i].loyalty);
  for (int i = 0; i < gEventCount; ++i) f.printf("event_%s=%d\n", gEvents[i].id.c_str(), gEvents[i].fired ? 1 : 0);
  for (int i = 0; i < gCampaignCount; ++i) f.printf("campaign_%s=%d\n", gCampaign[i].id.c_str(), gCampaign[i].completed ? 1 : 0);
  f.close();
  state.lastSaveMs = millis();
  state.saveDirty = false;
}

void loadState() {
  if (!state.sdOk || !SD.exists(savePath())) return;
  File f = SD.open(savePath(), FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = trimLine(f.readStringUntil('\n'));
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = trimLine(line.substring(0, eq));
    String value = trimLine(line.substring(eq + 1));
    if (key == "day") state.day = value.toInt();
    else if (key == "phase") state.phase = value.toInt();
    else if (key == "phase_minute") state.phaseMinute = value.toInt();
    else if (key == "heat_cooldown") state.heatCooldownMinutes = value.toInt();
    else if (key == "cash") state.cash = value.toInt();
    else if (key == "debt") state.debt = value.toInt();
    else if (key == "heat") state.heat = value.toInt();
    else if (key == "rep") state.rep = value.toInt();
    else if (key == "crew") state.crew = value.toInt();
    else if (key == "precursor") state.precursor = value.toInt();
    else if (key == "lab") state.lab = value.toInt();
    else if (key == "security") state.security = value.toInt();
    else if (key == "storage") state.storage = value.toInt();
    else if (key == "selected_product") state.selectedProduct = value.toInt();
    else if (key == "selected_district") state.selectedDistrict = value.toInt();
    else if (key == "selected_contact") state.selectedContact = value.toInt();
    else if (key == "campaign_stage") state.campaignStage = value.toInt();
    else if (key == "campaign_won") state.campaignWon = value.toInt() == 1;
    else if (key.startsWith("cooked_")) { int idx = key.substring(7).toInt(); if (idx >= 0 && idx < gProductCount) state.cooked[idx] = value.toInt(); }
    else if (key.startsWith("packed_")) { int idx = key.substring(7).toInt(); if (idx >= 0 && idx < gProductCount) state.packed[idx] = value.toInt(); }
    else if (key.startsWith("district_control_")) { int idx = key.substring(17).toInt(); if (idx >= 0 && idx < gDistrictCount) gDistricts[idx].control = value.toInt(); }
    else if (key.startsWith("district_sat_")) { int idx = key.substring(13).toInt(); if (idx >= 0 && idx < gDistrictCount) gDistricts[idx].saturation = value.toInt(); }
    else if (key.startsWith("contact_loyal_")) { int idx = key.substring(14).toInt(); if (idx >= 0 && idx < gContactCount) gContacts[idx].loyalty = value.toInt(); }
    else if (key.startsWith("event_")) { String id = key.substring(6); for (int i = 0; i < gEventCount; ++i) if (gEvents[i].id == id) gEvents[i].fired = (value.toInt() == 1); }
    else if (key.startsWith("campaign_")) { String id = key.substring(9); for (int i = 0; i < gCampaignCount; ++i) if (gCampaign[i].id == id) gCampaign[i].completed = (value.toInt() == 1); }
  }
  f.close();
  state.day = max(1, state.day);
  state.phase = clampStat(state.phase, 0, 2);
  state.phaseMinute = clampStat(state.phaseMinute, 0, kShiftLengthMinutes - 1);
  state.heatCooldownMinutes = clampStat(state.heatCooldownMinutes, 0, kPassiveHeatCooldownMinutes);
  clampSelections();
  stabilizeStats();
  state.saveDirty = false;
}

void validateContent() {
  if (gProductCount <= 0) {
    setDefaultProducts();
    state.banner = "Product data repaired.";
  }
  if (gCampaignCount <= 0) {
    setDefaultCampaign();
    state.banner = "Campaign data repaired.";
  }
  if (gDistrictCount <= 0) {
    setDefaultDistricts();
    state.banner = "District data repaired.";
  }
  if (gContactCount <= 0) {
    setDefaultContacts();
    state.banner = "Contact data repaired.";
  }
  if (gEventCount <= 0) {
    setDefaultEvents();
    state.banner = "Event data repaired.";
  }

  gProductCount = clampStat(gProductCount, 1, kMaxProducts);
  gDistrictCount = clampStat(gDistrictCount, 1, kMaxDistricts);
  gContactCount = clampStat(gContactCount, 1, kMaxContacts);
  gEventCount = clampStat(gEventCount, 1, kMaxEvents);
  gCampaignCount = clampStat(gCampaignCount, 1, kMaxCampaignStages);

  clampSelections();
  state.campaignStage = clampStat(state.campaignStage, 0, gCampaignCount - 1);
  state.dirty = true;
}

void clampSelections() {
  state.selectedProduct = clampStat(state.selectedProduct, 0, gProductCount - 1);
  state.selectedDistrict = clampStat(state.selectedDistrict, 0, gDistrictCount - 1);
  state.selectedContact = clampStat(state.selectedContact, 0, gContactCount - 1);
  state.districtPage = clampStat(state.districtPage, 0, pageCountFor(gDistrictCount) - 1);
  state.contactPage = clampStat(state.contactPage, 0, pageCountFor(gContactCount) - 1);
  int districtPageMin = state.districtPage * kVisibleCardCount;
  int districtPageMax = min(gDistrictCount - 1, districtPageMin + kVisibleCardCount - 1);
  if (state.selectedDistrict < districtPageMin || state.selectedDistrict > districtPageMax) {
    state.selectedDistrict = districtPageMin;
  }
  int contactPageMin = state.contactPage * kVisibleCardCount;
  int contactPageMax = min(gContactCount - 1, contactPageMin + kVisibleCardCount - 1);
  if (state.selectedContact < contactPageMin || state.selectedContact > contactPageMax) {
    state.selectedContact = contactPageMin;
  }
}

String repObjective() {
  if (state.rep < 12) return "Build steady cash without spiking heat.";
  if (state.rep < 20) return "Push control into Station Market.";
  if (state.rep < 30) return "Unlock Club Row and survive the pressure.";
  if (state.debt > 0) return "Clear the debt and hold the route.";
  return "Keep the network stable for one more week.";
}

int totalInventoryUnits() {
  int total = state.precursor;
  for (int i = 0; i < gProductCount; ++i) {
    total += state.cooked[i];
    total += state.packed[i];
  }
  return total;
}

int totalInventoryValue() {
  int total = state.precursor * 4;
  for (int i = 0; i < gProductCount; ++i) {
    total += state.cooked[i] * max(4, gProducts[i].basePrice / 2);
    total += state.packed[i] * gProducts[i].basePrice;
  }
  return total;
}

void wipeOperationalState() {
  state.day = 1;
  state.phase = 0;
  state.phaseMinute = 0;
  state.heatCooldownMinutes = 0;
  state.campaignStage = 0;
  state.campaignWon = false;
  state.cash = 180;
  state.debt = 120;
  state.heat = 8;
  state.rep = 0;
  state.crew = 1;
  state.precursor = 6;
  state.lab = 1;
  state.security = 1;
  state.storage = 8;
  state.selectedProduct = 0;
  state.selectedDistrict = 0;
  state.selectedContact = 0;
  state.districtPage = 0;
  state.contactPage = 0;
  state.showMission = false;
  state.logCount = 0;
  for (int i = 0; i < kMaxProducts; ++i) {
    state.cooked[i] = 0;
    state.packed[i] = 0;
  }
  for (int i = 0; i < gDistrictCount; ++i) {
    gDistricts[i].control = 0;
    gDistricts[i].saturation = 0;
  }
  for (int i = 0; i < gContactCount; ++i) {
    if (gContacts[i].role == "supplier") gContacts[i].loyalty = 52;
    else if (gContacts[i].role == "fixer") gContacts[i].loyalty = 58;
    else if (gContacts[i].role == "runner") gContacts[i].loyalty = 45;
    else if (gContacts[i].role == "scout") gContacts[i].loyalty = 47;
    else gContacts[i].loyalty = 40;
  }
  for (int i = 0; i < gEventCount; ++i) {
    gEvents[i].fired = false;
  }
  for (int i = 0; i < gCampaignCount; ++i) {
    gCampaign[i].completed = false;
  }
  state.banner = "New run. Keep it cleaner this time.";
  state.dirty = true;
  state.saveDirty = true;
}

void resetCampaign(const String& reason) {
  appendHistory(reason);
  wipeOperationalState();
  pushLog(reason);
  pushLog("The operation collapsed. Starting over.");
  saveState();
}

bool resolveBust(const ProductDef& p, DistrictDef& d, int amount) {
  int stockUnits = totalInventoryUnits();
  int stockValue = totalInventoryValue();
  int fine = 35 + amount * 10 + state.heat + stockUnits * 2 + stockValue / 40;
  fine = max(fine, 60);

  int seizedPacked = max(1, state.packed[state.selectedProduct] / 2);
  int seizedCooked = state.cooked[state.selectedProduct] / 3;
  int seizedPrecursor = min(state.precursor, 2 + state.lab);

  state.packed[state.selectedProduct] = max(0, state.packed[state.selectedProduct] - seizedPacked);
  state.cooked[state.selectedProduct] = max(0, state.cooked[state.selectedProduct] - seizedCooked);
  state.precursor = max(0, state.precursor - seizedPrecursor);
  d.control = max(0, d.control - max(3, amount));
  d.saturation = max(0, d.saturation - 6);
  state.rep = max(0, state.rep - (4 + p.risk / 6));
  state.heat = clampStat(state.heat + 18 + p.risk / 2 + d.risk / 3, 0, 100);

  if (state.cash >= fine) {
    state.cash -= fine;
    pushLog("BUSTED on " + p.label + ". Fine $" + String(fine) + ", heat spiked.");
    return true;
  }

  resetCampaign("BUSTED on " + p.label + ". Fine $" + String(fine) + " you could not pay.");
  return false;
}

void evaluateCampaignProgress() {
  if (state.campaignWon || gCampaignCount <= 0 || state.campaignStage >= gCampaignCount) return;
  CampaignStage& stage = gCampaign[state.campaignStage];
  if (stage.completed) {
    state.campaignStage = clampStat(state.campaignStage + 1, 0, gCampaignCount);
    if (state.campaignStage >= gCampaignCount) state.campaignWon = true;
    return;
  }

  if (state.day < stage.minDay) return;
  if (state.rep < stage.minRep) return;
  if (state.cash < stage.minCash) return;
  if (state.heat > stage.maxHeat) return;
  if (state.debt > stage.maxDebt) return;
  if (maxDistrictControl() < stage.minControl) return;

  stage.completed = true;
  state.cash += stage.rewardCash;
  state.rep += stage.rewardRep;
  state.lab += stage.rewardLab;
  state.crew += stage.rewardCrew;
  state.storage += stage.rewardStorage;
  if (!stage.unlockDistrictId.isEmpty()) {
    int idx = findDistrictIndexById(stage.unlockDistrictId);
    if (idx >= 0) {
      gDistricts[idx].unlockRep = min(gDistricts[idx].unlockRep, state.rep);
    }
  }
  pushLog("Campaign cleared: " + stage.title + ".");
  if (!stage.story.isEmpty()) {
    pushLog(stage.story);
  }
  state.campaignStage++;
  if (state.campaignStage >= gCampaignCount) {
    state.campaignStage = gCampaignCount - 1;
    state.campaignWon = true;
    pushLog("You reached the top. Free play unlocked.");
  } else {
    pushLog("New chapter: " + gCampaign[state.campaignStage].title + ".");
  }
}

bool districtUnlocked(int idx) { return idx >= 0 && idx < gDistrictCount && state.rep >= gDistricts[idx].unlockRep; }

void applyEvent(int idx) {
  if (idx < 0 || idx >= gEventCount || gEvents[idx].fired) return;
  EventDef& e = gEvents[idx];
  e.fired = true;
  state.cash += e.cashDelta; state.heat += e.heatDelta; state.rep += e.repDelta; state.debt += e.debtDelta;
  if (e.id == "supplier_credit") state.precursor += 3;
  pushLog(e.text);
  appendHistory(e.text);
}

void rollEvent() {
  int candidates[kMaxEvents];
  int count = 0;
  for (int i = 0; i < gEventCount; ++i) {
    const EventDef& e = gEvents[i];
    if (!e.fired && state.day >= e.minDay && state.heat >= e.minHeat && state.rep >= e.minRep) candidates[count++] = i;
  }
  if (count > 0) applyEvent(candidates[random(count)]);
}

void stabilizeStats() {
  state.heat = clampStat(state.heat, 0, 100); state.rep = clampStat(state.rep, 0, 100); state.cash = max(0, state.cash); state.debt = max(0, state.debt);
  state.crew = clampStat(state.crew, 1, 9); state.precursor = clampStat(state.precursor, 0, 999);
  state.heatCooldownMinutes = clampStat(state.heatCooldownMinutes, 0, kPassiveHeatCooldownMinutes);
  for (int i = 0; i < gProductCount; ++i) { state.cooked[i] = clampStat(state.cooked[i], 0, 999); state.packed[i] = clampStat(state.packed[i], 0, 999); }
  for (int i = 0; i < gDistrictCount; ++i) { gDistricts[i].control = clampStat(gDistricts[i].control, 0, 99); gDistricts[i].saturation = clampStat(gDistricts[i].saturation, 0, 99); }
  for (int i = 0; i < gContactCount; ++i) gContacts[i].loyalty = clampStat(gContacts[i].loyalty, 10, 95);
}

void applyPassiveHeatCooldown(int minutes) {
  if (minutes <= 0) return;
  if (state.heat <= 0) {
    state.heatCooldownMinutes = 0;
    return;
  }
  int cooldownStep = max(70, kPassiveHeatCooldownMinutes - state.security * 10);
  state.heatCooldownMinutes += minutes;
  int decay = state.heatCooldownMinutes / cooldownStep;
  if (decay <= 0) return;
  state.heat = max(0, state.heat - decay);
  state.heatCooldownMinutes = state.heatCooldownMinutes % cooldownStep;
  if (state.heat <= 0) {
    state.heatCooldownMinutes = 0;
  }
}

void advanceWorldTime(int minutes) {
  if (minutes <= 0) return;
  applyPassiveHeatCooldown(minutes);
  state.phaseMinute += minutes;
  while (state.phaseMinute >= kShiftLengthMinutes) {
    state.phaseMinute -= kShiftLengthMinutes;
    endTurn();
  }
  state.dirty = true;
  state.saveDirty = true;
}

void endTurn() {
  state.phase++;
  if (state.phase >= 3) {
    state.phase = 0; state.day++; state.debt += 4 + max(1, state.debt / 50); state.cash = max(0, state.cash - (state.crew * 6));
    for (int i = 0; i < gDistrictCount; ++i) { gDistricts[i].saturation = max(0, gDistricts[i].saturation - 2); if (random(100) < 25) gDistricts[i].control = max(0, gDistricts[i].control - 1); }
    for (int i = 0; i < gContactCount; ++i) { int drift = random(-2, 3); if (random(100) < gContacts[i].volatility) drift -= 1; gContacts[i].loyalty = clampStat(gContacts[i].loyalty + drift, 10, 95); }
    state.heat = max(0, state.heat - (4 + state.security + (state.crew <= 2 ? 1 : 0)));
    if (random(100) < 60) rollEvent(); else pushLog("New day. The city is still hungry.");
  } else {
    state.heat = max(0, state.heat - (1 + state.security / 2));
    if (state.heat > 22 && random(100) < 30) pushLog("Patrols feel closer tonight."); else pushLog(String("Shift changed to ") + kPhaseNames[state.phase] + ".");
  }
  if (state.heat >= 70) { state.cash = max(0, state.cash - 15); pushLog("Heat is burning cash just to keep doors open."); }
  if (state.heat >= 95) {
    resetCampaign("The route cooked off under impossible heat.");
    return;
  }
  stabilizeStats();
  evaluateCampaignProgress();
  appendHistory(state.banner);
  saveState();
}
ActionSlot makeAction(ActionId id, const char* label) {
  ActionSlot slot;
  slot.id = id;
  slot.label = label;
  return slot;
}

void setActions(const ActionSlot& a0, const ActionSlot& a1, const ActionSlot& a2, const ActionSlot& a3, const ActionSlot& a4, const ActionSlot& a5) {
  gActions[0] = a0; gActions[1] = a1; gActions[2] = a2; gActions[3] = a3; gActions[4] = a4; gActions[5] = a5;
}

String shortDistrictStatus(const DistrictDef& d) {
  int idx = static_cast<int>(&d - gDistricts);
  if (!districtUnlocked(idx)) return "LOCKED";
  if (d.saturation > 14) return "SATURATED";
  if (d.risk > 16) return "HOT";
  if (d.control > 8) return "OWNED";
  return "OPEN";
}

String shortContactStatus(const ContactDef& c) {
  if (c.loyalty > 70) return "SOLID";
  if (c.loyalty < 30) return "SHAKY";
  if (c.volatility > 24) return "RISKY";
  return "READY";
}

String fitText(const String& text, int maxWidth, int font = 1) {
  if (text.isEmpty()) return "";
  if (tft.textWidth(text, font) <= maxWidth) return text;
  String out = text;
  while (out.length() > 1 && tft.textWidth(out + "...", font) > maxWidth) {
    out.remove(out.length() - 1);
  }
  return out + "...";
}

void drawFittedText(int x, int y, const String& text, int maxWidth, uint16_t fg, uint16_t bg, int font = 1) {
  tft.setTextColor(fg, bg);
  tft.setTextFont(font);
  tft.setCursor(x, y);
  tft.print(fitText(text, maxWidth, font));
}

void drawFittedTwoLine(int x, int y, const String& text, int maxWidth, uint16_t fg, uint16_t bg, int font = 1, int lineGap = 10) {
  String working = text;
  working.trim();
  if (working.isEmpty()) {
    return;
  }
  int split = -1;
  for (int i = working.length() - 1; i >= 0; --i) {
    if (working.charAt(i) == ' ') {
      String first = working.substring(0, i);
      if (tft.textWidth(first, font) <= maxWidth) {
        split = i;
        break;
      }
    }
  }
  if (split < 0) {
    drawFittedText(x, y, working, maxWidth, fg, bg, font);
    return;
  }
  String first = working.substring(0, split);
  String second = working.substring(split + 1);
  drawFittedText(x, y, first, maxWidth, fg, bg, font);
  drawFittedText(x, y + lineGap, second, maxWidth, fg, bg, font);
}

void drawWrappedTextBlock(int x, int y, const String& text, int maxWidth, int maxLines, uint16_t fg, uint16_t bg, int font = 1, int lineGap = 10) {
  String remaining = text;
  remaining.trim();
  int line = 0;
  while (!remaining.isEmpty() && line < maxLines) {
    if (tft.textWidth(remaining, font) <= maxWidth || line == maxLines - 1) {
      drawFittedText(x, y + line * lineGap, remaining, maxWidth, fg, bg, font);
      break;
    }
    int split = -1;
    for (int i = remaining.length() - 1; i >= 0; --i) {
      if (remaining.charAt(i) == ' ') {
        String first = remaining.substring(0, i);
        if (tft.textWidth(first, font) <= maxWidth) {
          split = i;
          break;
        }
      }
    }
    if (split < 0) {
      drawFittedText(x, y + line * lineGap, remaining, maxWidth, fg, bg, font);
      break;
    }
    String current = remaining.substring(0, split);
    remaining = remaining.substring(split + 1);
    remaining.trim();
    drawFittedText(x, y + line * lineGap, current, maxWidth, fg, bg, font);
    line++;
  }
}

void drawHeader() {
  tft.fillRect(0, 0, kScreenWidth, 22, kPanel);
  tft.drawFastHLine(0, 22, kScreenWidth, kAccent);
  tft.setTextFont(2); tft.setTextSize(1);
  drawFittedText(8, 6, state.title, 108, kText, kPanel, 2);
  drawFittedText(132, 6, currentClockText(), 54, kAccent2, kPanel, 2);
  tft.setTextColor(kAccent2, kPanel); tft.setCursor(194, 6); tft.printf("D%02d", state.day); tft.setCursor(230, 6); tft.print(kPhaseNames[state.phase]); tft.setCursor(286, 6); tft.print(state.sdOk ? "SD" : "MEM");
}

void drawStatsStrip() {
  tft.fillRect(0, 24, kScreenWidth, 30, kPanelAlt);
  tft.drawFastHLine(0, 54, kScreenWidth, kAccent);
  drawFittedText(8, 29, "$" + String(state.cash), 52, kText, kPanelAlt, 1);
  drawFittedText(56, 29, "Debt " + String(state.debt), 80, kText, kPanelAlt, 1);
  drawFittedText(142, 29, "Heat " + String(state.heat), 74, state.heat > 45 ? kBad : (state.heat > 25 ? kWarn : kGood), kPanelAlt, 1);
  drawFittedText(222, 29, "Rep " + String(state.rep), 54, kAccent2, kPanelAlt, 1);
  drawFittedText(8, 41, "Crew " + String(state.crew) + "  Lab " + String(state.lab) + "  Pre " + String(state.precursor), 210, kMuted, kPanelAlt, 1);
}

void drawTabs() {
  for (int i = 0; i < 5; ++i) {
    bool active = static_cast<int>(state.page) == i;
    uint16_t bg = active ? kAccent : kPanel;
    uint16_t fg = active ? kBg : kText;
    tft.fillRoundRect(kTabs[i].x, kTabs[i].y, kTabs[i].w, kTabs[i].h, 4, bg);
    tft.drawRoundRect(kTabs[i].x, kTabs[i].y, kTabs[i].w, kTabs[i].h, 4, kPanelAlt);
    tft.setTextColor(fg, bg);
    int tx = kTabs[i].x + (kTabs[i].w - tft.textWidth(kTabs[i].label, 1)) / 2;
    tft.setTextFont(1);
    tft.setCursor(tx, kTabs[i].y + 8);
    tft.print(kTabs[i].label);
  }
}

void drawActionGrid() {
  for (int i = 0; i < kActionCount; ++i) {
    bool enabled = gActions[i].id != ActionId::None && gActions[i].label.length() > 0;
    uint16_t bg = enabled ? kPanel : kBg;
    uint16_t fg = enabled ? kText : kMuted;
    tft.fillRoundRect(kActionButtons[i].x, kActionButtons[i].y, kActionButtons[i].w, kActionButtons[i].h, 4, bg);
    tft.drawRoundRect(kActionButtons[i].x, kActionButtons[i].y, kActionButtons[i].w, kActionButtons[i].h, 4, kAccent);
    tft.setTextColor(fg, bg);
    String label = fitText(gActions[i].label, kActionButtons[i].w - 10, 1);
    int tx = kActionButtons[i].x + (kActionButtons[i].w - tft.textWidth(label, 1)) / 2;
    tft.setTextFont(1);
    tft.setCursor(tx, kActionButtons[i].y + 8);
    tft.print(label);
  }
}

void drawBodyBase(const char* title) {
  tft.fillRect(0, 56, kScreenWidth, 154, kBg);
  drawFittedText(8, 60, title, 110, kAccent2, kBg, 1);
  drawFittedText(8, 73, state.banner, 304, kMuted, kBg, 1);
}

void drawHomePage() {
  drawBodyBase("OVERVIEW");
  setActions(makeAction(ActionId::PrevProduct, "PREV"), makeAction(ActionId::NextProduct, "NEXT"), makeAction(ActionId::CoolHeat, "COOL"), makeAction(ActionId::PayDebt, "PAY"), makeAction(ActionId::EndTurn, "END"), makeAction(ActionId::SaveGame, "SAVE"));
  const ProductDef& p = gProducts[state.selectedProduct];
  const DistrictDef& d = gDistricts[state.selectedDistrict];
  drawFittedText(10, 88, "Product: " + p.label + " " + String(state.selectedProduct + 1) + "/" + String(gProductCount), 150, kText, kBg, 1);
  drawFittedText(10, 100, "Cooked " + String(state.cooked[state.selectedProduct]) + "  Packed " + String(state.packed[state.selectedProduct]), 150, kText, kBg, 1);
  drawFittedText(10, 112, "Focus: " + d.label, 150, kText, kBg, 1);
  drawFittedText(10, 124, "Status: " + shortDistrictStatus(d), 150, kText, kBg, 1);
  tft.drawRoundRect(170, 84, 140, 58, 5, kAccent);
  drawFittedText(178, 95, currentChapterTitle(), 124, kAccent2, kBg, 1);
  drawFittedTwoLine(178, 108, currentObjective(), 124, kText, kBg, 1, 10);
  tft.drawRoundRect(10, 146, 300, 10, 4, kPanelAlt);
  int heatBar = map(state.heat, 0, 100, 0, 296);
  tft.fillRoundRect(12, 148, heatBar, 6, 3, state.heat > 45 ? kBad : (state.heat > 25 ? kWarn : kGood));
}

void drawOpsPage() {
  drawBodyBase("OPERATIONS");
  setActions(makeAction(ActionId::PrevProduct, "PREV"), makeAction(ActionId::NextProduct, "NEXT"), makeAction(ActionId::BuyPrecursors, "BUY PRE"), makeAction(ActionId::Cook, "COOK"), makeAction(ActionId::Pack, "PACK"), makeAction(ActionId::UpgradeLab, "UPGRADE"));
  const ProductDef& p = gProducts[state.selectedProduct];
  tft.drawRoundRect(10, 88, 300, 58, 5, kAccent);
  drawFittedText(18, 96, p.label + " / risk " + String(p.risk) + " / cost " + String(p.chemCost), 284, kAccent2, kBg, 1);
  drawFittedText(18, 110, p.desc, 284, kText, kBg, 1);
  String reqText = "Req L" + String(p.requiredLab) + " R" + String(p.requiredRep);
  if (state.lab < p.requiredLab || state.rep < p.requiredRep) {
    reqText += " LOCK";
  }
  drawFittedText(18, 124, reqText + " / Cooked " + String(state.cooked[state.selectedProduct]) + " Packed " + String(state.packed[state.selectedProduct]) + " / $" + String(p.basePrice), 284, (state.lab < p.requiredLab || state.rep < p.requiredRep) ? kWarn : kMuted, kBg, 1);
}

void drawMapPage() {
  drawBodyBase("DISTRICTS");
  setActions(makeAction(ActionId::PrevProduct, "PREV"), makeAction(ActionId::NextProduct, "NEXT"), makeAction(ActionId::Deliver, "DELIVER"), makeAction(ActionId::Bribe, "BRIBE"), makeAction(ActionId::EndTurn, "END"), makeAction(ActionId::SaveGame, "SAVE"));
  int start = state.districtPage * kVisibleCardCount;
  int visible = min(kVisibleCardCount, gDistrictCount - start);
  for (int slot = 0; slot < visible; ++slot) {
    int i = start + slot;
    int col = slot % 2; int row = slot / 2; int x = 10 + col * 154; int y = 88 + row * 31;
    bool selected = state.selectedDistrict == i; uint16_t bg = selected ? kAccent : kPanel; uint16_t fg = selected ? kBg : kText;
    tft.fillRoundRect(x, y, 146, 26, 4, bg); tft.drawRoundRect(x, y, 146, 26, 4, kAccent2); tft.setTextColor(fg, bg);
    drawFittedText(x + 7, y + 7, gDistricts[i].label, 88, fg, bg, 1);
    drawFittedText(x + 100, y + 7, shortDistrictStatus(gDistricts[i]), 40, fg, bg, 1);
  }
  const DistrictDef& d = gDistricts[state.selectedDistrict]; const ProductDef& p = gProducts[state.selectedProduct];
  drawFittedText(10, 154, "Dem " + String(d.demand[state.selectedProduct]) + " / Ctrl " + String(d.control) + " / Sat " + String(d.saturation), 170, kMuted, kBg, 1);
  if (pageCountFor(gDistrictCount) > 1) {
    int px = 232;
    int py = 148;
    tft.fillRoundRect(px, py, 16, 16, 3, kPanel);
    tft.drawRoundRect(px, py, 16, 16, 3, kAccent2);
    drawFittedText(px + 5, py + 4, "<", 6, kAccent2, kPanel, 1);
    tft.fillRoundRect(px + 20, py, 40, 16, 3, kPanelAlt);
    tft.drawRoundRect(px + 20, py, 40, 16, 3, kAccent2);
    drawFittedText(px + 26, py + 4, String(state.districtPage + 1) + "/" + String(pageCountFor(gDistrictCount)), 28, kText, kPanelAlt, 1);
    tft.fillRoundRect(px + 64, py, 16, 16, 3, kPanel);
    tft.drawRoundRect(px + 64, py, 16, 16, 3, kAccent2);
    drawFittedText(px + 69, py + 4, ">", 6, kAccent2, kPanel, 1);
  }
}

void drawContactsPage() {
  drawBodyBase("CONTACTS");
  setActions(makeAction(ActionId::Meet, "MEET"), makeAction(ActionId::Ask, "ASK"), makeAction(ActionId::Recruit, "HIRE"), makeAction(ActionId::PayDebt, "PAY"), makeAction(ActionId::EndTurn, "END"), makeAction(ActionId::SaveGame, "SAVE"));
  int start = state.contactPage * kVisibleCardCount;
  int visible = min(kVisibleCardCount, gContactCount - start);
  for (int slot = 0; slot < visible; ++slot) {
    int i = start + slot;
    int col = slot % 2; int row = slot / 2; int x = 10 + col * 154; int y = 88 + row * 31;
    bool selected = state.selectedContact == i; uint16_t bg = selected ? kAccent : kPanel; uint16_t fg = selected ? kBg : kText;
    tft.fillRoundRect(x, y, 146, 26, 4, bg); tft.drawRoundRect(x, y, 146, 26, 4, kAccent2); tft.setTextColor(fg, bg);
    drawFittedText(x + 7, y + 7, gContacts[i].label, 88, fg, bg, 1);
    drawFittedText(x + 100, y + 7, shortContactStatus(gContacts[i]), 40, fg, bg, 1);
  }
  const ContactDef& c = gContacts[state.selectedContact];
  drawFittedText(10, 154, c.role + " / Loy " + String(c.loyalty) + " / Vol " + String(c.volatility), 180, kMuted, kBg, 1);
  if (pageCountFor(gContactCount) > 1) {
    int px = 232;
    int py = 148;
    tft.fillRoundRect(px, py, 16, 16, 3, kPanel);
    tft.drawRoundRect(px, py, 16, 16, 3, kAccent2);
    drawFittedText(px + 5, py + 4, "<", 6, kAccent2, kPanel, 1);
    tft.fillRoundRect(px + 20, py, 40, 16, 3, kPanelAlt);
    tft.drawRoundRect(px + 20, py, 40, 16, 3, kAccent2);
    drawFittedText(px + 26, py + 4, String(state.contactPage + 1) + "/" + String(pageCountFor(gContactCount)), 28, kText, kPanelAlt, 1);
    tft.fillRoundRect(px + 64, py, 16, 16, 3, kPanel);
    tft.drawRoundRect(px + 64, py, 16, 16, 3, kAccent2);
    drawFittedText(px + 69, py + 4, ">", 6, kAccent2, kPanel, 1);
  }
}

void drawLogPage() {
  drawBodyBase("INTEL");
  setActions(makeAction(ActionId::LoadGame, "LOAD"), makeAction(ActionId::SaveGame, "SAVE"), makeAction(ActionId::CoolHeat, "COOL"), makeAction(ActionId::PayDebt, "PAY"), makeAction(ActionId::EndTurn, "END"), makeAction(ActionId::ShowMission, state.showMission ? "BACK" : "MISSION"));
  if (state.showMission) {
    drawFittedText(10, 89, currentChapterTitle(), 304, kAccent2, kBg, 1);
    drawWrappedTextBlock(10, 102, currentObjective(), 304, 4, kText, kBg, 1, 10);
    if (!state.campaignWon && state.campaignStage < gCampaignCount) {
      drawWrappedTextBlock(10, 146, gCampaign[state.campaignStage].story, 304, 2, kMuted, kBg, 1, 10);
    } else {
      drawWrappedTextBlock(10, 146, state.tip, 304, 2, kMuted, kBg, 1, 10);
    }
  } else {
    for (int i = 0; i < state.logCount && i < 6; ++i) {
      drawFittedText(10, 89 + i * 11, state.logs[i], 304, i == 0 ? kText : kMuted, kBg, 1);
    }
  }
}

void render() {
  drawHeader(); drawStatsStrip();
  switch (state.page) {
    case Page::Home: drawHomePage(); break;
    case Page::Ops: drawOpsPage(); break;
    case Page::Map: drawMapPage(); break;
    case Page::Contacts: drawContactsPage(); break;
    case Page::Log: drawLogPage(); break;
  }
  drawActionGrid(); drawTabs(); state.dirty = false;
}

void applySelectedContactDrift(ContactDef& contact, int delta) {
  contact.loyalty = clampStat(contact.loyalty + delta, 10, 95);
}

void doBuyPrecursors() {
  int cost = 18 + state.day * 2;
  if (state.cash < cost) {
    pushLog("Not enough cash for precursor.");
    return;
  }
  state.cash -= cost;
  state.precursor += 4 + state.lab;
  pushLog("Bought precursor stock.");
  if (gContactCount > 0) {
    applySelectedContactDrift(gContacts[0], 2);
  }
  advanceWorldTime(12);
}

void doCook() {
  ProductDef& p = gProducts[state.selectedProduct];
  if (state.lab < p.requiredLab) {
    pushLog(p.label + " needs LAB " + String(p.requiredLab) + ".");
    return;
  }
  if (state.rep < p.requiredRep) {
    pushLog(p.label + " needs REP " + String(p.requiredRep) + ".");
    return;
  }
  int batches = min(state.precursor / p.chemCost, max(1, state.lab));
  if (batches <= 0) {
    pushLog("No precursor to cook.");
    return;
  }
  state.precursor -= batches * p.chemCost;
  state.cooked[state.selectedProduct] += batches * p.batchYield;
  state.heat += p.heatGain;
  pushLog("Cooked a fresh batch of " + p.label + ".");
  advanceWorldTime(35);
}

void doPack() {
  int amount = min(state.cooked[state.selectedProduct], 3 + state.crew);
  if (amount <= 0) {
    pushLog("Nothing cooked to pack.");
    return;
  }
  state.cooked[state.selectedProduct] -= amount;
  state.packed[state.selectedProduct] += amount;
  pushLog("Packed " + String(amount) + " units.");
  advanceWorldTime(20);
}

void doDeliver() {
  ProductDef& p = gProducts[state.selectedProduct];
  DistrictDef& d = gDistricts[state.selectedDistrict];
  ContactDef& c = gContacts[state.selectedContact];
  if (!districtUnlocked(state.selectedDistrict)) {
    pushLog("District still locked.");
    return;
  }
  int amount = min(state.packed[state.selectedProduct], max(1, 2 + state.crew / 2));
  if (amount <= 0) {
    pushLog("No packed units ready.");
    return;
  }

  int basePerUnit = p.basePrice + d.demand[state.selectedProduct] + d.control / 2 + (c.loyalty - 40) / 3;
  basePerUnit -= d.saturation / 2;
  basePerUnit = max(6, basePerUnit);
  int payout = amount * basePerUnit;
  int heatGain = max(1, p.risk + d.risk / 3 - c.loyalty / 25);

  state.packed[state.selectedProduct] -= amount;
  state.cash += payout;
  state.rep += max(1, amount + d.demand[state.selectedProduct] / 8);
  state.heat += heatGain;
  d.saturation = clampStat(d.saturation + amount * 2, 0, 100);
  d.control = clampStat(d.control + 2, 0, 100);

  int bustChance = 0;
  if (state.heat >= 28) {
    bustChance = state.heat / 2;
    bustChance += d.risk;
    bustChance += amount * 4;
    bustChance += c.volatility / 2;
    bustChance -= c.loyalty / 4;
    bustChance -= state.security * 4;
    bustChance = clampStat(bustChance, 0, 88);
  }

  if (bustChance > 0 && random(100) < bustChance) {
    if (!resolveBust(p, d, amount)) {
      return;
    }
    applySelectedContactDrift(c, -6);
  } else if (random(100) < c.volatility) {
    int skim = max(6, payout / 6);
    state.cash -= skim;
    pushLog(c.label + " skimmed $" + String(skim) + ".");
    applySelectedContactDrift(c, -4);
  } else {
    pushLog("Drop landed in " + d.label + " for $" + String(payout) + ".");
    applySelectedContactDrift(c, 2);
  }
  advanceWorldTime(26);
}

void doBribe() {
  DistrictDef& d = gDistricts[state.selectedDistrict];
  int cost = 30 + d.risk * 2;
  if (state.cash < cost) {
    pushLog("No cash to smooth the block.");
    return;
  }
  state.cash -= cost;
  state.heat = max(0, state.heat - 10);
  d.saturation = max(0, d.saturation - 8);
  d.control = clampStat(d.control + 3, 0, 100);
  pushLog("Pressure cooled in " + d.label + ".");
  advanceWorldTime(15);
}

void doRecruit() {
  int cost = 45 + state.crew * 18;
  if (state.cash < cost) {
    pushLog("Recruit wants more than you have.");
    return;
  }
  if (state.rep < 8 + state.crew * 4) {
    pushLog("Your name is still too small.");
    return;
  }
  state.cash -= cost;
  state.crew += 1;
  state.heat += 2;
  pushLog("A new runner joined the roster.");
  advanceWorldTime(20);
}

void doUpgradeLab() {
  int cost = 55 + state.lab * 25;
  if (state.cash < cost) {
    pushLog("Upgrade cost too high.");
    return;
  }
  state.cash -= cost;
  state.lab += 1;
  state.storage += 4;
  pushLog("Lab capacity upgraded.");
  advanceWorldTime(30);
}

void doMeet() {
  ContactDef& c = gContacts[state.selectedContact];
  int drift = random(-3, 5);
  applySelectedContactDrift(c, drift);
  if (drift >= 0) {
    pushLog("Meeting with " + c.label + " went clean.");
  } else {
    pushLog(c.label + " left irritated.");
  }
  advanceWorldTime(10);
}

void doAsk() {
  ContactDef& c = gContacts[state.selectedContact];
  if (c.role == "supplier") {
    state.precursor += 3;
    state.debt += 12;
    pushLog(c.label + " floated stock on credit.");
  } else if (c.role == "fixer") {
    state.heat = max(0, state.heat - 6);
    pushLog(c.label + " buried a rumor.");
  } else if (c.role == "runner") {
    gDistricts[state.selectedDistrict].control = clampStat(gDistricts[state.selectedDistrict].control + 3, 0, 100);
    pushLog(c.label + " mapped a cleaner route.");
  } else if (c.role == "scout") {
    gDistricts[state.selectedDistrict].saturation = max(0, gDistricts[state.selectedDistrict].saturation - 5);
    pushLog(c.label + " found a quiet corner.");
  } else {
    state.rep += 2;
    pushLog(c.label + " wants premium service soon.");
  }
  applySelectedContactDrift(c, 2);
  advanceWorldTime(10);
}

void doPayDebt() {
  if (state.debt <= 0) {
    pushLog("No debt due right now.");
    return;
  }
  int payment = min(state.debt, min(state.cash, 50));
  if (payment <= 0) {
    pushLog("No cash to pay debt.");
    return;
  }
  state.cash -= payment;
  state.debt -= payment;
  pushLog("Paid $" + String(payment) + " toward debt.");
  advanceWorldTime(5);
}

void doCoolHeat() {
  int cost = 18;
  if (state.cash < cost) {
    pushLog("Cooling the block still costs cash.");
    return;
  }
  state.cash -= cost;
  state.heat = max(0, state.heat - 7);
  pushLog("You went quiet for a night.");
  advanceWorldTime(20);
}

void activateAction(ActionId action) {
  switch (action) {
    case ActionId::PrevProduct:
      state.selectedProduct = (state.selectedProduct + gProductCount - 1) % gProductCount;
      pushLog("Focused " + gProducts[state.selectedProduct].label + ".");
      break;
    case ActionId::NextProduct:
      state.selectedProduct = (state.selectedProduct + 1) % gProductCount;
      pushLog("Focused " + gProducts[state.selectedProduct].label + ".");
      break;
    case ActionId::BuyPrecursors: doBuyPrecursors(); break;
    case ActionId::Cook: doCook(); break;
    case ActionId::Pack: doPack(); break;
    case ActionId::Deliver: doDeliver(); break;
    case ActionId::Bribe: doBribe(); break;
    case ActionId::Recruit: doRecruit(); break;
    case ActionId::UpgradeLab: doUpgradeLab(); break;
    case ActionId::Meet: doMeet(); break;
    case ActionId::Ask: doAsk(); break;
    case ActionId::PayDebt: doPayDebt(); break;
    case ActionId::CoolHeat: doCoolHeat(); break;
    case ActionId::EndTurn: advanceWorldTime(max(1, kShiftLengthMinutes - state.phaseMinute)); break;
    case ActionId::SaveGame: saveState(); pushLog("State saved."); break;
    case ActionId::LoadGame: loadState(); pushLog("State loaded."); break;
    case ActionId::ShowMission:
      state.showMission = !state.showMission;
      state.banner = state.showMission ? "Mission detail opened." : "Back to intel log.";
      break;
    case ActionId::None: default: return;
  }
  stabilizeStats();
  evaluateCampaignProgress();
  state.dirty = true;
}

void handlePageSelection(int x, int y) {
  if (state.page == Page::Map) {
    if (pageCountFor(gDistrictCount) > 1 && within(x, y, 232, 148, 80, 16)) {
      if (x < 250 && state.districtPage > 0) {
        state.districtPage--;
      } else if (x > 294 && state.districtPage < pageCountFor(gDistrictCount) - 1) {
        state.districtPage++;
      }
      clampSelections();
      state.banner = gDistricts[state.selectedDistrict].desc;
      state.dirty = true;
      return;
    }
    int start = state.districtPage * kVisibleCardCount;
    int visible = min(kVisibleCardCount, gDistrictCount - start);
    for (int slot = 0; slot < visible; ++slot) {
      int i = start + slot;
      int col = slot % 2;
      int row = slot / 2;
      int bx = 10 + col * 154;
      int by = 88 + row * 31;
      if (within(x, y, bx, by, 146, 26)) {
        state.selectedDistrict = i;
        state.banner = gDistricts[i].desc;
        state.dirty = true;
        return;
      }
    }
  }

  if (state.page == Page::Contacts) {
    if (pageCountFor(gContactCount) > 1 && within(x, y, 232, 148, 80, 16)) {
      if (x < 250 && state.contactPage > 0) {
        state.contactPage--;
      } else if (x > 294 && state.contactPage < pageCountFor(gContactCount) - 1) {
        state.contactPage++;
      }
      clampSelections();
      state.banner = gContacts[state.selectedContact].desc;
      state.dirty = true;
      return;
    }
    int start = state.contactPage * kVisibleCardCount;
    int visible = min(kVisibleCardCount, gContactCount - start);
    for (int slot = 0; slot < visible; ++slot) {
      int i = start + slot;
      int col = slot % 2;
      int row = slot / 2;
      int bx = 10 + col * 154;
      int by = 88 + row * 31;
      if (within(x, y, bx, by, 146, 26)) {
        state.selectedContact = i;
        state.banner = gContacts[i].desc;
        state.dirty = true;
        return;
      }
    }
  }
}

void handleTouch() {
  static uint32_t lastTouch = 0;
  if (!touch.touched()) {
    return;
  }
  if (millis() - lastTouch < kTouchDebounceMs) {
    return;
  }
  lastTouch = millis();

  CYD28_TS_Point p = touch.getPointScaled();
  int16_t x = p.x;
  int16_t y = p.y;
  if (x < 0 || y < 0 || x > kScreenWidth || y > kScreenHeight) {
    return;
  }

  for (int i = 0; i < 5; ++i) {
    if (within(x, y, kTabs[i].x, kTabs[i].y, kTabs[i].w, kTabs[i].h)) {
      state.page = static_cast<Page>(i);
      if (state.page != Page::Log) state.showMission = false;
      clampSelections();
      if (state.page == Page::Map) state.banner = gDistricts[state.selectedDistrict].desc;
      if (state.page == Page::Contacts) state.banner = gContacts[state.selectedContact].desc;
      state.dirty = true;
      return;
    }
  }

  handlePageSelection(x, y);

  for (int i = 0; i < kActionCount; ++i) {
    if (within(x, y, kActionButtons[i].x, kActionButtons[i].y, kActionButtons[i].w, kActionButtons[i].h)) {
      activateAction(gActions[i].id);
      return;
    }
  }
}

void loadAllContent() {
  setDefaultProducts();
  setDefaultDistricts();
  setDefaultContacts();
  setDefaultEvents();
  setDefaultCampaign();
  loadConfigFile();
  loadProductsFile();
  loadDistrictsFile();
  loadContactsFile();
  loadEventsFile();
  loadCampaignFile();
  validateContent();
  state.contentOk = true;
}

bool mountSd() {
  sdSpi.begin(18, 19, 23, 5);
  if (!SD.begin(5, sdSpi)) {
    state.banner = "SD missing. Insert /deadend pack.";
    state.contentOk = false;
    return false;
  }
  state.sdOk = true;
  ensureDir("/deadend");
  ensureDir("/deadend/world");
  ensureDir("/deadend/products");
  ensureDir("/deadend/characters");
  ensureDir("/deadend/events");
  ensureDir("/deadend/campaign");
  ensureDir("/deadend/mods");
  ensureDir("/deadend/mods/products");
  ensureDir("/deadend/mods/districts");
  ensureDir("/deadend/mods/characters");
  ensureDir("/deadend/mods/events");
  ensureDir("/deadend/mods/campaign");
  ensureDir("/deadend/system");
  ensureDir("/deadend/saves");
  return true;
}

void setupEngine() {
  randomSeed(static_cast<uint32_t>(esp_random()));
  setBacklight(true);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(kBg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(2);
  touch.begin();
  touch.setRotation(1);

  state.page = Page::Home;
  state.day = 1;
  state.phase = 0;
  state.phaseMinute = 0;
  state.cash = 180;
  state.debt = 120;
  state.precursor = 6;
  state.crew = 1;
  state.lab = 1;
  state.storage = 8;
  state.heat = 8;
  state.rep = 0;
  state.selectedProduct = 0;
  state.selectedDistrict = 0;
  state.selectedContact = 0;
  state.districtPage = 0;
  state.contactPage = 0;
  state.showMission = false;
  state.campaignStage = 0;
  state.campaignWon = false;
  state.logCount = 0;
  state.banner = "Build quiet. Stay alive.";
  state.dirty = true;

  bool sdOk = mountSd();
  state.sdOk = sdOk;
  loadAllContent();
  if (sdOk) {
    loadState();
  } else {
    setDefaultProducts();
    setDefaultDistricts();
    setDefaultContacts();
    setDefaultEvents();
  }

  if (state.logCount == 0) {
    pushLog("Dead End Inc. online.");
    pushLog("Start small. Avoid noise.");
  }
  saveState();
}

}  // namespace deadend

void setup() {
  Serial.begin(115200);
  delay(120);
  deadend::setupEngine();
}

void loop() {
  static uint32_t lastClockTick = millis();
  deadend::handleTouch();
  if (millis() - lastClockTick > deadend::kAutoClockTickMs) {
    deadend::advanceWorldTime(deadend::kAutoClockStepMinutes);
    lastClockTick = millis();
  }
  if (deadend::state.dirty) {
    deadend::render();
  }
  static uint32_t lastAutoSave = 0;
  if (deadend::state.sdOk && deadend::state.saveDirty && millis() - lastAutoSave > deadend::kAutoSaveMs) {
    deadend::saveState();
    lastAutoSave = millis();
  }
  delay(20);
}
