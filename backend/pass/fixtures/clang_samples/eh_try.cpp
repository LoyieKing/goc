int goc_eh_try(int x) {
  try { if (x) throw x; } catch (int) { return -1; }
  return 0;
}
