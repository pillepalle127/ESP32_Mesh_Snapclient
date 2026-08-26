# Patch anwenden

Vorher einen Testbranch anlegen:

```powershell
git checkout main
git pull
git checkout -b test/stability-patch
```

Den Inhalt des Ordners `stability_patch` über den Projektstamm kopieren. Dabei die vorhandenen Dateien ersetzen.

Danach:

```powershell
idf.py fullclean
idf.py reconfigure
idf.py build
```

Nur bei erfolgreichem Build flashen:

```powershell
idf.py -p COM5 flash monitor
```

Änderungen zunächst nicht direkt nach `main` committen. Die Testmatrix steht in `TESTING.md`.
