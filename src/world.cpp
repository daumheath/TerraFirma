/**
 * @Copyright 2015 seancode
 *
 * Handles the loading and storage of a world file
 */

#include <QFile>
#include <QDir>
#include <QDebug>
#include <utility>
#include <zlib.h>
#include "world.h"
#include "worldinfo.h"

World::World(QObject *parent) : QObject(parent) {
  setAutoDelete(false);  // please don't kill me!
  tiles = nullptr;
  tilesWide = tilesHigh = 0;
}

World::~World() {
  delete [] tiles;
}

void World::init() {
  try {
    info.init();
  } catch (WorldInfo::InitException &e) {
    throw InitException("Failed to init definitions", e.reason);
  }

  try {
    header.init();
  } catch (WorldHeader::InitException &e) {
    throw InitException("Failed to init header", e.reason);
  }
}

void World::setFilename(QString filename) {
  this->filename = std::move(filename);
}

void World::setPlayer(QString filename) {
  this->player = std::move(filename);
  if (!this->filename.isEmpty())
    loadPlayer();
}

void World::run() {
  auto handle = QSharedPointer<Handle>(new Handle(filename));

  int version = handle->r32();
  if (version > HighestVersion) {
    emit loadError("Unsupported map version: " + QString::number(version));
    return;
  }
  if (version < MinimumVersion) {
    emit loadError("We no longer support maps this old");
    return;
  }

  if (version >= 135) {
    QString magic = handle->read(7);
    if (magic != "relogic") {
      emit loadError("Not a relogic map file");
      return;
    }
    quint8 type = handle->r8();
    if (type != 2) {
      emit loadError("Not a map file");
      return;
    }
    handle->skip(4 + 8);  // revision + favorites
  }
  int numSections = handle->r16();
  QList<int> sections;
  for (int i = 0; i < numSections; i++)
    sections.append(handle->r32());
  int numTiles = handle->r16();
  quint8 mask = 0x80;
  quint8 bits = 0;
  QList<bool> extra;
  for (int i = 0; i < numTiles; i++) {
    if (mask == 0x80) {
      bits = handle->r8();
      mask = 1;
    } else {
      mask <<= 1;
    }
    extra.append(bits & mask);
  }

  handle->seek(sections[0]);  // skip any extra junk
  loadHeader(handle, version);
  handle->seek(sections[1]);
  loadTiles(handle, version, extra);
  handle->seek(sections[2]);
  loadChests(handle, version);
  handle->seek(sections[3]);
  loadSigns(handle, version);
  handle->seek(sections[4]);
  loadNPCs(handle, version);
  handle->seek(sections[5]);
  if (version >= 116) {
    if (version < 122)
      loadDummies(handle, version);
    else
      loadEntities(handle, version);
  }
  if (version >= 170) {
    handle->seek(sections[6]);
    loadPressurePlates(handle, version);
  }
  if (version >= 189) {
    handle->seek(sections[7]);
    loadTownManager(handle, version);
  }
  if (version >= 210) {
    handle->seek(sections[8]);
    loadBestiary(handle, version);
  }
  if (version >= 220) {
    handle->seek(sections[9]);
    loadCreativePowers(handle, version);
  }

  if (!player.isEmpty())
    loadPlayer();

  // spreadLight();
  emit loaded(true);
}

void World::loadHeader(QSharedPointer<Handle> handle, int version) {
  header.load(std::move(handle), version);

  tilesHigh = header["tilesHigh"]->toInt();
  tilesWide = header["tilesWide"]->toInt();

  tiles = new Tile[tilesWide * tilesHigh];
}

void World::loadTiles(const QSharedPointer<Handle> &handle, int version,
                      const QList<bool> &extra) {
  for (int x = 0; x < tilesWide; x++) {
    emit status(tr("Reading tiles: %1%").arg(
                  static_cast<int>(x * 100.0f / tilesWide)));
    int offset = x;
    for (int y = 0; y < tilesHigh; y++) {
      int rle = tiles[offset].load(handle, version, extra);

      int destOffset = offset + tilesWide;
      for (int r = 0; r < rle; r++, destOffset += tilesWide)
        memcpy(&tiles[destOffset], &tiles[offset], sizeof(Tile));
      y += rle;
      offset = destOffset;
    }
  }
}

void World::loadChests(const QSharedPointer<Handle> &handle, int) {
  chests.clear();
  emit status("Loading Chests...");
  int numChests = handle->r16();
  int itemsPerChest = handle->r16();
  for (int i = 0; i < numChests; i++) {
    Chest chest;
    chest.x = handle->r32();
    chest.y = handle->r32();
    chest.name = handle->rs();
    for (int j = 0; j < itemsPerChest; j++) {
      int stack = handle->r16();
      if (stack > 0) {
        Chest::Item item;
        item.stack = stack;
        item.name = info.items[handle->r32()];
        item.prefix = info.prefixes[handle->r8()];
        chest.items.append(item);
      }
    }
    chests.append(chest);
  }
}

void World::loadSigns(const QSharedPointer<Handle> &handle, int) {
  signs.clear();
  emit status("Loading Signs...");
  int numSigns = handle->r16();
  for (int i = 0; i < numSigns; i++) {
    Sign sign;
    sign.text = handle->rs();
    sign.x = handle->r32();
    sign.y = handle->r32();
    signs.append(sign);
  }
}

void World::loadNPCs(const QSharedPointer<Handle> &handle, int version) {
  npcs.clear();
  emit status("Loading NPCs...");
  if (version >= 268) {
    int num = handle->r32();
    for (int i = 0; i < num; i++) {
      shimmered[handle->r32()] = true;
    }
  }
  while (handle->r8()) {
    NPC npc;
    npc.head = 0;
    npc.sprite = 0;
    if (version >=190) {
        npc.sprite = handle->r32();
        if (info.npcsById.contains(npc.sprite)) {
          auto theNPC = info.npcsById[npc.sprite];
          npc.head = theNPC->head;
          npc.title = theNPC->title;
        }
    } else {
        npc.title = handle->rs();
        if (info.npcsByName.contains(npc.title)) {
          auto theNPC = info.npcsByName[npc.title];
          npc.head = theNPC->head;
          npc.sprite = theNPC->id;
        }
    }
    npc.name = handle->rs();
    npc.x = handle->rf();
    npc.y = handle->rf();
    npc.homeless = handle->r8();
    npc.homeX = handle->r32();
    npc.homeY = handle->r32();
    if (version >= 213 && handle->r8()) {
      npc.townVariation = handle->r32();
    }
    npcs.append(npc);
  }
  if (version >= 140) {
    while (handle->r8()) {
      NPC npc;
      if (version >=190) {
          npc.sprite = handle->r32();
          if (info.npcsById.contains(npc.sprite)) {
            auto theNPC = info.npcsById[npc.sprite];
            npc.title = theNPC->title;
          }
      } else {
          npc.title = handle->rs();
          if (info.npcsByName.contains(npc.title)) {
            auto theNPC = info.npcsByName[npc.title];
            npc.sprite = theNPC->id;
          }
      }
      npc.name = "";
      npc.x = handle->rf();
      npc.y = handle->rf();
      npc.homeless = true;
      npcs.append(npc);
    }
  }
}

void World::loadDummies(const QSharedPointer<Handle> &handle, int) {
  int numDummies = handle->r32();
  for (int i = 0; i < numDummies; i++) {
    handle->r16();  // x
    handle->r16();  // y
    // do we need this?  Is anyone running this
    // version of the map?
  }
}

void World::loadEntities(const QSharedPointer<Handle> &handle, int) {
  entities.clear();
  int numEntities = handle->r32();
  for (int i = 0; i < numEntities; i++) {
    int type = handle->r8();
    switch (type) {
    case 0: {
      TrainingDummy dummy;
      dummy.id = handle->r32();
      dummy.x = handle->r16();
      dummy.y = handle->r16();
      dummy.npc = handle->r16();
      entities.append(dummy);
    }
      break;
    case 1: {
      ItemFrame frame;
      frame.id = handle->r32();
      frame.x = handle->r16();
      frame.y = handle->r16();
      frame.itemid = handle->r16();
      frame.prefix = handle->r8();
      frame.stack = handle->r16();
      entities.append(frame);
    }
      break;
    case 2: {
      LogicSensor sensor;
      sensor.id = handle->r32();
      sensor.x = handle->r16();
      sensor.y = handle->r16();
      sensor.type = handle->r8();
      sensor.on = handle->r8() != 0;
      entities.append(sensor);
    }
    }
  }
}

void World::loadPressurePlates(const QSharedPointer<Handle> &handle, int) {
  int numPlates = handle->r32();
  for (int i = 0; i < numPlates; i++) {
    handle->r32();  //x
    handle->r32();  //y
  }
}

void World::loadTownManager(const QSharedPointer<Handle> &handle, int) {
  int numRooms = handle->r32();
  for (int i = 0; i < numRooms; i++) {
    handle->r32();  //NPC
    handle->r32();  //X
    handle->r32();  //Y
    // I wonder if they will eventually depreciate the 'home' location in the NPC data.
    // This data is for the new feature where NPC's remember which room they were in before they died
  }
}

void World::loadBestiary(const QSharedPointer<Handle> &handle, int) {
  int numKills = handle->r32();
  for (int i = 0; i < numKills; i++) {
    auto npc = handle->rs();
    kills[npc] = handle->r32();
  }
  int numSights = handle->r32();
  for (int i = 0; i < numSights; i++) {
    seen.append(handle->rs());
  }
  int numChat = handle->r32();
  for (int i = 0; i < numChat; i++) {
    chats.append(handle->rs());
  }
}

void World::loadCreativePowers(const QSharedPointer<Handle> &, int) {
  // do we care if biome spread is disabled and stuff?
  // let's ignore this for now.
}

void World::spreadLight() {
  /*
  // step 1, set light sources
  int offset = 0;
  for (int y = 0; y < tilesHigh; y++) {
    emit status(tr("Lighting tiles : %1%").arg(
        static_cast<int>(y * 50.0f / tilesHigh)), 0);
    for (int x = 0; x < tilesWide; x++, offset++) {
      auto tile = &tiles[offset];
      auto inf = info[tile];
      if ((!tile->active() || inf->transparent) &&
          (tile->wall == 0 || tile->wall == 21) &&
          tile->liquid < 255 && y < header["groundLevel"]->toInt())
        // sunlit
        tile->setLight(1.0, 1.0, 1.0);
      else
        tile->setLight(0.0, 0.0, 0.0);
      if (tile->liquid > 0 && tile->lava())
        tile->addLight(0.66, 0.39, 0.13);
      tile->addLight(inf->lightR, inf->lightG, inf->lightB);

      double delta = 0.04;
      if (tile->active() && !inf->transparent)
        delta = 0.16;
      if (y > 0) {
        auto prev = &tiles[offset - tilesWide];
        tile->addLight(prev->lightR() - delta,
                       prev->lightG() - delta,
                       prev->lightB() - delta);
      }
      if (x > 0) {
        auto prev = &tiles[offset - 1];
        tile->addLight(prev->lightR() - delta,
                       prev->lightG() - delta,
                       prev->lightB() - delta);
      }
    }
  }
  // step 2, spread light backwards
  offset = tilesHigh * tilesWide - 1;
  for (int y = tilesHigh - 1; y >= 0; y--) {
    emit status(tr("Spreading light: %1%").arg(
        static_cast<int>((tilesHigh - y) * 50.0f / tilesHigh + 50)), 0);
    for (int x = tilesWide - 1; x >= 0; x--, offset--) {
      auto tile = &tiles[offset];
      auto inf = info[tile];
      double delta = 0.04;
      if (tile->active() && !inf->transparent)
        delta = 0.16;
      if (y < tilesHigh - 1) {
        auto prev = &tiles[offset + tilesWide];
        tile->addLight(prev->lightR() - delta,
                       prev->lightG() - delta,
                       prev->lightB() - delta);
      }
      if (x < tilesWide - 1) {
        auto prev = &tiles[offset + 1];
        tile->addLight(prev->lightR() - delta,
                       prev->lightG() - delta,
                       prev->lightB() - delta);
      }
    }
  }*/
}

void World::loadPlayer() {
  QDir dir;

    // try guid first
  QString path = player.left(player.lastIndexOf("."));
  if (header.has("guid")) {
    auto g = header["guid"];
    quint32 u1 = g->at(0)->toInt() |
        (g->at(1)->toInt() << 8) |
        (g->at(2)->toInt() << 16) |
        (g->at(3)->toInt() << 24);
    quint16 u2 = g->at(4)->toInt() |
        (g->at(5)->toInt() << 8);
    quint16 u3 = g->at(6)->toInt() |
        (g->at(7)->toInt() << 8);
    quint16 u4 = (g->at(8)->toInt() << 8) |
        g->at(9)->toInt();
    quint16 u5 = (g->at(10)->toInt() << 8) |
        g->at(11)->toInt();
    quint32 u6 = (g->at(12)->toInt() << 24) |
        (g->at(13)->toInt() << 16) |
        (g->at(14)->toInt() << 8) |
        g->at(15)->toInt();

    path += QDir::toNativeSeparators(
          QString("/%1-%2-%3-%4-%5%6.map")
          .arg(u1, 8, 16, QChar('0'))
          .arg(u2, 4, 16, QChar('0'))
          .arg(u3, 4, 16, QChar('0'))
          .arg(u4, 4, 16, QChar('0'))
          .arg(u5, 4, 16, QChar('0'))
          .arg(u6, 8, 16, QChar('0')));
  }
  if (!header.has("guid") || !dir.exists(path)) {
    // try old way of worldID
    path = player.left(player.lastIndexOf("."));
    path += QDir::toNativeSeparators(QString("/%1.map")
                                     .arg(header["worldID"]->toInt()));
  }

  if (!dir.exists(path)) {
    int offset = 0;
    for (int y = 0; y < tilesHigh; y++) {
      for (int x = 0; x < tilesWide; x++, offset++) {
        tiles[offset].setSeen(true);
      }
    }
    return;
  }

  auto handle = QSharedPointer<Handle>(new Handle(path));
  int version = handle->r32();
  if (version <= 91)
    loadPlayer1(handle, version);
  else
    loadPlayer2(handle, version);
}

void World::loadPlayer1(QSharedPointer<Handle> handle, int version) {
  handle->rs();  // name
  handle->r32();  // id
  handle->r32();  // tiles high
  handle->r32();  // tiles wide
  for (int x = 0; x < tilesWide; x++) {
    int offset = x;
    for (int y = 0; y < tilesHigh; y++, offset += tilesWide) {
      if (handle->r8()) {
        if (version <= 77)
          handle->r8();  // tileid
        else
          handle->r16();  // tileid
        handle->r8();  // light
        handle->r8();  // misc
        if (version >= 50) handle->r8();  // misc2
        tiles[offset].setSeen(true);
        int rle = handle->r16();
        while (rle-- > 0) {
          y++; offset += tilesWide;
          tiles[offset].setSeen(true);
        }
      } else {
        int rle = handle->r16();
        while (rle-- > 0) {
          y++; offset += tilesWide;
          tiles[offset].setSeen(false);
        }
      }
    }
  }
}

void World::loadPlayer2(QSharedPointer<Handle> handle, int version) {
  if (version >= 135) {
    QString magic = handle->read(7);
    if (magic != "relogic") {
      emit loadError("Not a relogic map file");
      return;
    }
    quint8 type = handle->r8();
    if (type != 1) {
      emit loadError("Not a player map file");
      return;
    }
    handle->skip(4 + 8);  // revision + favorites
  }

  handle->rs();  // name
  handle->r32();  // worldid
  handle->r32();  // tiles high
  handle->r32();  // tiles wide

  int numTiles = handle->r16();
  int numWalls = handle->r16();
  handle->r16();  // num unk1
  handle->r16();  // num unk2
  handle->r16();  // num unk3
  handle->r16();  // num unk4

  QList<bool> tilePresent;
  quint8 mask = 0x80;
  quint8 bits = 0;
  for (int i = 0; i < numTiles; i++) {
    if (mask == 0x80) {
      bits = handle->r8();
      mask = 1;
    } else {
      mask <<= 1;
    }
    tilePresent.append(bits & mask);
  }

  QList<bool> wallPresent;
  mask = 0x80;
  bits = 0;
  for (int i = 0; i < numWalls; i++) {
    if (mask == 0x80) {
      bits = handle->r8();
      mask = 1;
    } else {
      mask <<= 1;
    }
    wallPresent.append(bits & mask);
  }

  for (int i = 0; i < numTiles; i++) {
    if (tilePresent[i])
      handle->r8();  // throw away tile data
  }
  for (int i = 0; i < numWalls; i++) {
    if (wallPresent[i])
      handle->r8();  // throw away wall data
  }

  if (version >= 93) {
    QByteArray output;
    z_stream strm;
    static const int CHUNK_SIZE = 32768;
    char out[CHUNK_SIZE];
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = handle->length() - handle->tell();
    strm.next_in = (Bytef *)handle->readBytes(handle->length()
                                              - handle->tell());

    inflateInit2(&strm, -15);
    do {
      strm.avail_out = CHUNK_SIZE;
      strm.next_out = reinterpret_cast<Bytef *>(out);
      inflate(&strm, Z_NO_FLUSH);
      output.append(out, CHUNK_SIZE - strm.avail_out);
    } while (strm.avail_out == 0);
    inflateEnd(&strm);
    handle = QSharedPointer<Handle>(new Handle(output));
  }

  int offset = 0;
  for (int y = 0; y < tilesHigh; y++) {
    for (int x = 0; x < tilesWide; x++, offset++) {
      quint8 flags = handle->r8();
      if (flags & 1) handle->r8();  // color
      quint8 tile = (flags >> 1) & 7;
      if (tile == 1 || tile == 2 || tile == 7) {
        if (flags & 16)
          handle->r16();  // tileid
        else
          handle->r8();  // tileid
      }
      quint8 light = (flags & 32) ? handle->r8() : 255;

      int rle = 0;
      switch ((flags >> 6) & 3) {
      case 1: rle = handle->r8(); break;
      case 2: rle = handle->r16(); break;
      }

      if (tile) {
        tiles[offset].setSeen(true);
        if (light == 255) {
          while (rle-- > 0) {
            x++;
            tiles[++offset].setSeen(true);
          }
        } else {
          while (rle-- > 0) {
            x++;
            handle->r8();  // light
            tiles[++offset].setSeen(true);
          }
        }
      } else {
        tiles[offset].setSeen(false);
        while (rle-- > 0) {
          x++;
          tiles[++offset].setSeen(false);
        }
      }
    }
  }
}


int Tile::load(const QSharedPointer<Handle> &handle, int, const QList<bool> &extra) {
  quint8 flags1 = handle->r8(), flags2 = 0, flags3 = 0;
  if (flags1 & 1) {  // has flags2
    flags2 = handle->r8();
    if (flags2 & 1)  // has flags3
      flags3 = handle->r8();
  }
  bool active = flags1 & 2;
  flags = active ? 1 : 0;
  if (active) {
    type = handle->r8();
    if (flags1 & 0x20)  // 2-byte type
      type |= handle->r8() << 8;
    if (extra[type]) {
      u = handle->r16();
      v = handle->r16();
    } else {
      u = v = -1;
    }
    if (flags3 & 0x8)
      color = handle->r8();
  } else {
    type = 0;
  }
  if (flags1 & 4) {  // wall
    wall = handle->r8();
    if (flags3 & 0x10)
      wallColor = handle->r8();
    wallu = wallv = -1;
  } else {
    wall = 0;
  }
  if (flags1 & 0x18) {
    liquid = handle->r8();
    if ((flags1 & 0x18) == 0x10)  // lava
      flags |= 2;
    if ((flags1 & 0x18) == 0x18)  // honey
      flags |= 4;
    if (flags3 & 0x80) {  // shimmer
      flags |= 0x800;
    }
  } else {
    liquid = 0;
  }
  if (flags2 & 2)  // red wire
    flags |= 8;
  if (flags2 & 4)  // blue wire
    flags |= 0x10;
  if (flags2 & 8)  // green wire
    flags |= 0x20;
  int slop = (flags2 >> 4) & 7;
  if (slop == 1)  // half
    flags |= 0x40;
  slope = slop > 1 ? slop - 1 : 0;

  if (flags3 & 2)  // actuator
    flags |= 0x80;
  if (flags3 & 4)  // inactive
    flags |= 0x100;
  if (flags3 & 32)  // yellow wire
    flags |= 0x400;
  if (flags3 & 64)  // wall is word
    wall |= handle->r8() << 8;

  int rle = 0;
  switch (flags1 >> 6) {
  case 1:
    rle = handle->r8();
    break;
  case 2:
    rle = handle->r16();
    break;
  }
  return rle;
}


bool Tile::active() const {
  return flags & 1;
}

bool Tile::lava() const {
  return flags & 2;
}

bool Tile::honey() const {
  return flags & 4;
}

bool Tile::shimmer() const {
  return flags & 0x800;
}

bool Tile::redWire() const {
  return flags & 8;
}

bool Tile::blueWire() const {
  return flags & 0x10;
}

bool Tile::greenWire() const {
  return flags & 0x20;
}

bool Tile::half() const {
  return flags & 0x40;
}

bool Tile::actuator() const {
  return flags & 0x80;
}

bool Tile::inactive() const {
  return flags & 0x100;
}

bool Tile::seen() const {
  return flags & 0x200;
}

void Tile::setSeen(bool seen) {
  if (seen)
    flags |= 0x200;
  else
    flags &= ~0x200;
}

bool Tile::yellowWire() const {
  return flags & 0x400;
}
