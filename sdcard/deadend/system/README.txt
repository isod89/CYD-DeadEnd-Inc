Dead End Inc. modding pack

Base files:
/world/config.cfg
/world/districts.txt
/products/products.txt
/characters/contacts.txt
/events/events.txt
/campaign/campaign.txt
/saves/slot1.cfg
/saves/history.log

Auto-loaded mod folders:
/mods/products/
/mods/districts/
/mods/characters/
/mods/events/
/mods/campaign/

Any text file dropped into those /mods folders is loaded automatically at boot.
If an entry uses an existing id, it overrides that id.
If an entry uses a new id, it is appended until the engine limit is reached.

Formats:
products = id|label|basePrice|risk|cookCost|batchYield|heatGain|requiredLab|requiredRep|desc
districts = id|label|risk|unlockRep|demand1|demand2|demand3|...|demand24|desc
contacts = id|label|role|districtId|loyalty|volatility|favoriteProductId|desc
events = id|minDay|minHeat|minRep|text|cashDelta|heatDelta|repDelta|debtDelta
campaign = id|title|objective|minDay|minRep|minCash|maxHeat|maxDebt|minControl|rewardCash|rewardRep|rewardLab|rewardCrew|rewardStorage|unlockDistrictId|story

Limits in this build:
products 24
districts 12
contacts 16
events 32
campaign stages 16

UI behavior:
- product focus cycles with PREV/NEXT
- districts page when there are more than 4 districts
- contacts page when there are more than 4 contacts
- campaign objectives appear automatically if campaign content exists

Fallback behavior:
- invalid or empty categories fall back to firmware defaults
- duplicate ids override the previous definition
- old product files without requiredLab/requiredRep still work; the firmware derives defaults automatically
