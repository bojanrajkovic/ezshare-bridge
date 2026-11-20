#pragma once

// Registers /files/ (index pages, GET and HEAD of copied files), /metrics, /log and
// POST /restart, then starts the server on port 80.
void startWeb();

void handleWeb();
