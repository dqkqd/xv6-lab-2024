struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  struct buf *lprev; // LRU cache list
  struct buf *lnext;
  struct buf *hprev; // Hash queue
  struct buf *hnext;
  uchar data[BSIZE];
};

