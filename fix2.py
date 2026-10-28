path = r'D:\Projetos\edopro\gframe\game.cpp'
with open(path, encoding='utf-8', errors='ignore') as f:
    c = f.read()
old = 'gSettings.chkAutoSaveReplay = env->addCheckBox(gGameConfig->autoSaveReplay, GetNextRect(), sPanel, CHECKBOX_AUTO_SAVE_REPLAY, gDataManager->GetSysString(12116).data());\r\n\t\tdefaultStrings.emplace_back(gSettings.chkAutoSaveReplay, 12116);'
new = 'gSettings.chkAutoSaveReplay = env->addCheckBox(gGameConfig->autoSaveReplay, GetNextRect(), sPanel, CHECKBOX_AUTO_SAVE_REPLAY, L"Salvar replays automaticamente");'
if old in c:
    c2 = c.replace(old, new)
    with open(path, 'w', encoding='utf-8', errors='ignore') as f:
        f.write(c2)
    print('ok')
else:
    print('no')
