#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <vector>

using namespace geode::prelude;

enum { K_HAZARD = 0, K_BLOCK = 1, K_SLOPE = 2, K_HSLOPE = 3 };

struct Obstacle {
	float x0, y0, x1, y1;   // габарит
	int kind;
	// для наклонов: точка A на гипотенузе и нормаль (в сторону твёрдой части)
	float ax, ay, nx, ny;
};

static float speedUnits(float m) {
	if (m < 0.8f) return 251.16f;
	if (m < 1.0f) return 311.58f;
	if (m < 1.2f) return 387.42f;
	if (m < 1.45f) return 468.0f;
	return 576.0f;
}

// пересекается ли квадрат (центр x,y; полуразмер rr) с препятствием
static bool overlapsObs(Obstacle const& o, float x, float y, float rr) {
	if (!(x > o.x0 - rr && x < o.x1 + rr && y > o.y0 - rr && y < o.y1 + rr)) return false;
	if (o.kind == K_SLOPE || o.kind == K_HSLOPE) {
		float fc = o.nx * (x - o.ax) + o.ny * (y - o.ay);
		return fc + rr * (std::abs(o.nx) + std::abs(o.ny)) > 0.f;
	}
	return true;
}

class $modify(BotLayer, GJBaseGameLayer) {
	struct Fields {
		std::vector<Obstacle> obstacles; // отсортированы по x0
		std::vector<Obstacle> window;
		float maxW = 30.f;
		bool built = false;

		// куб
		bool holding = false;
		float releaseTimer = 0.f;
		float w0 = -1.f;
		float jitter = 0.f;
		bool willMiss = false;

		// волна
		bool waveHold = false;
		bool pending = false;
		float delay = 0.f;
		bool hasPrev = false;
		float prevX = 0.f, prevY = 0.f;
		float slope = 1.f;
		float upSign = 1.f;

		// отладка
		struct Dbg {
			int mode = 0;
			int firstBad = -1, b = -1;
			float w0 = -1.f, thr = 0.f;
			int dUp = -1, dDown = -1;
			bool jumped = false, emergency = false;
			float jx = 0.f, jy = 0.f;
		} dbg;
		bool deadLogged = false;

		std::mt19937 rng{std::random_device{}()};
	};

	void buildObjects() {
		auto f = m_fields.self();
		f->obstacles.clear();
		for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
			auto type = obj->m_objectType;
			bool isSlope = type == GameObjectType::Slope;
			bool isSolid = type == GameObjectType::Solid;
			bool isHazard = type == GameObjectType::Hazard;
			if (!isSlope && !isSolid && !isHazard) continue;
			if (obj->m_isNoTouch) continue;

			auto r = obj->getObjectRect();
			Obstacle o{r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY(), K_BLOCK, 0.f, 0.f, 0.f, 0.f};
			if (isHazard) o.kind = K_HAZARD;

			if (isSlope) {
				o.kind = obj->m_slopeIsHazard ? K_HSLOPE : K_SLOPE;

				// базовый наклон: прямой угол справа внизу, потом отражения и поворот
				float sx = 1.f, sy = -1.f;
				if (obj->isFlipX()) sx = -sx;
				if (obj->isFlipY()) sy = -sy;
				int q = ((static_cast<int>(std::lround(obj->getRotation() / 90.f)) % 4) + 4) % 4;
				for (int j = 0; j < q; ++j) {
					float t = sx;
					sx = sy;
					sy = -t;
				}
				float cx = sx > 0.f ? o.x1 : o.x0;
				float cy = sy > 0.f ? o.y1 : o.y0;

				// сверяем со штатной линией наклона из игры
				float ya = static_cast<float>(obj->slopeYPos(o.x0));
				float yb = static_cast<float>(obj->slopeYPos(o.x1));
				bool okLine = ya > o.y0 - 2.f && ya < o.y1 + 2.f && yb > o.y0 - 2.f && yb < o.y1 + 2.f;
				if (okLine) {
					float yAtC = (cx == o.x0) ? ya : yb;
					if (std::abs(cy - yAtC) < 2.f) cx = (cx == o.x0) ? o.x1 : o.x0;
				}

				float ox = (cx == o.x0) ? o.x1 : o.x0;
				float oy = (cy == o.y0) ? o.y1 : o.y0;
				// гипотенуза: A=(ox, cy) -> B=(cx, oy)
				float dx = cx - ox, dy = oy - cy;
				float len = std::sqrt(dx * dx + dy * dy);
				if (len < 0.001f) continue;
				float nx = -dy / len, ny = dx / len;
				if (nx * (cx - ox) + ny * 0.f < 0.f) { nx = -nx; ny = -ny; }
				o.ax = ox;
				o.ay = cy;
				o.nx = nx;
				o.ny = ny;
			}

			f->obstacles.push_back(o);
			if (r.size.width < 400.f) f->maxW = std::max(f->maxW, r.size.width);
		}
		std::sort(f->obstacles.begin(), f->obstacles.end(),
			[](Obstacle const& a, Obstacle const& b) { return a.x0 < b.x0; });
		f->built = true;
	}

	void logDeath() {
		auto f = m_fields.self();
		float px = m_player1->getPositionX();
		float py = m_player1->getPositionY();
		auto const& d = f->dbg;
		log::info("[AutoJump] DEAD mode={} pos=({:.1f},{:.1f}) ground={} speedMul={:.2f} legit={}",
			d.mode, px, py, m_player1->m_isOnGround, m_player1->m_playerSpeed,
			Mod::get()->getSettingValue<int64_t>("legit"));
		log::info("[AutoJump] plan: firstBad={} b={} w0={:.1f} thr={:.1f} dUp={} dDown={} slope={:.2f} upSign={:.0f} jumped={} jumpAt=({:.1f},{:.1f}) emergency={}",
			d.firstBad, d.b, d.w0, d.thr, d.dUp, d.dDown, f->slope, f->upSign,
			d.jumped, d.jx, d.jy, d.emergency);
		int n = 0;
		for (auto const& o : f->obstacles) {
			if (o.x1 < px - 60.f || o.x0 > px + 80.f) continue;
			if (o.y1 < py - 80.f || o.y0 > py + 80.f) continue;
			log::info("[AutoJump]   obstacle kind={} rect=({:.1f},{:.1f})-({:.1f},{:.1f})",
				o.kind, o.x0, o.y0, o.x1, o.y1);
			if (++n >= 10) break;
		}
	}

	// ================= ВОЛНА =================
	void waveLogic(float dt) {
		auto f = m_fields.self();
		int legit = static_cast<int>(Mod::get()->getSettingValue<int64_t>("legit"));

		float px = m_player1->getPositionX();
		float py = m_player1->getPositionY();

		if (f->hasPrev) {
			float ddx = px - f->prevX;
			float ddy = py - f->prevY;
			if (ddx > 0.1f && std::abs(ddy) > 0.3f * ddx) {
				f->slope = std::clamp(0.7f * f->slope + 0.3f * std::abs(ddy) / ddx, 0.5f, 3.f);
				f->upSign = ((ddy > 0.f) == f->waveHold) ? 1.f : -1.f;
			}
		}

		constexpr int N = 28;
		constexpr int KMAX = N + 6;
		constexpr int OFF = KMAX;
		constexpr int W = 2 * KMAX + 1;
		constexpr int DEAD = -1000;

		float speed = speedUnits(m_player1->m_playerSpeed);
		float H = speed * 0.55f;
		float dx = H / N;
		float step = f->slope * dx;
		float r = std::clamp(5.f * std::abs(m_player1->getScale()), 3.f, 6.f);
		float yRange = (N + 6) * step + 40.f;

		f->window.clear();
		auto it = std::lower_bound(f->obstacles.begin(), f->obstacles.end(),
			px - f->maxW - 10.f,
			[](Obstacle const& o, float v) { return o.x0 < v; });
		for (; it != f->obstacles.end() && it->x0 <= px + H + 20.f; ++it) {
			if (it->x1 < px - 10.f) continue;
			if (it->y1 < py - yRange || it->y0 > py + yRange) continue;
			f->window.push_back(*it);
		}

		struct Hit { int kind = 0; Obstacle const* o = nullptr; };
		// kind: 0 свободно, 1 смерть (шип), 2 блок, 3 наклон
		auto hit = [&](float x, float y, float rr) {
			Hit h;
			for (auto const& o : f->window) {
				if (!overlapsObs(o, x, y, rr)) continue;
				if (o.kind == K_HAZARD || o.kind == K_HSLOPE) { h.kind = 1; return h; }
				if (h.kind == 0) { h.kind = (o.kind == K_SLOPE) ? 3 : 2; h.o = &o; }
			}
			return h;
		};

		auto move = [&](int i, int k, bool up, float rr) -> int {
			float x = px + (i + 1) * dx;
			int nk = k + (up ? 1 : -1);
			if (nk < -KMAX || nk > KMAX) return DEAD;
			Hit h = hit(x, py + nk * step, rr);
			if (h.kind == 0) return nk;
			if (h.kind == 1) return DEAD;
			if (h.kind == 2) {
				// блок: скользим по нему, если уже были над/под ним
				return hit(x, py + k * step, rr).kind == 0 ? k : DEAD;
			}
			// наклон: скользим вдоль него, если подошли со свободной стороны
			Obstacle const& o = *h.o;
			float fPrev = o.nx * ((x - dx) - o.ax) + o.ny * ((py + k * step) - o.ay);
			if (fPrev > 0.f) return DEAD; // врезались в торец
			int dir = (o.ny < 0.f) ? 1 : -1; // твёрдое снизу -> уходим вверх
			for (int s = 0; s <= 5; ++s) {
				int ck = nk + dir * s;
				if (ck < -KMAX || ck > KMAX) break;
				if (hit(x, py + ck * step, rr).kind == 0) return ck;
			}
			return DEAD;
		};

		auto plan = [&](float rr, int& dUp, int& dDown) {
			std::array<std::array<int, W>, N + 1> d{};
			for (int i = N - 1; i >= 1; --i) {
				int lim = std::min(i + 6, KMAX);
				for (int k = -lim; k <= lim; ++k) {
					int best = 0;
					for (int dir = 0; dir < 2; ++dir) {
						int nk = move(i, k, dir == 1, rr);
						if (nk != DEAD) best = std::max(best, 1 + d[i + 1][nk + OFF]);
					}
					d[i][k + OFF] = best;
				}
			}
			int nu = move(0, 0, true, rr);
			int nd = move(0, 0, false, rr);
			dUp = (nu != DEAD) ? 1 + d[1][nu + OFF] : 0;
			dDown = (nd != DEAD) ? 1 + d[1][nd + OFF] : 0;
		};

		int dUp = 0, dDown = 0;
		float tiers[3] = {r + 5.f, r + 2.5f, r};
		for (float rr : tiers) {
			plan(rr, dUp, dDown);
			if (std::max(dUp, dDown) >= N) break;
		}

		f->dbg.mode = 2;
		f->dbg.dUp = dUp;
		f->dbg.dDown = dDown;
		bool holdMeansUp = f->upSign > 0.f;
		bool curUp = (f->waveHold == holdMeansUp);
		bool goUp = curUp;
		if (dUp > dDown) goUp = true;
		else if (dDown > dUp) goUp = false;
		bool want = (goUp == holdMeansUp);

		if (want != f->waveHold) {
			if (!f->pending) {
				f->pending = true;
				if (legit > 0) {
					std::uniform_real_distribution<float> dist(0.f, legit * 0.012f);
					f->delay = dist(f->rng);
				} else {
					f->delay = 0.f;
				}
			}
			f->delay -= dt;
			if (f->delay > 0.f) want = f->waveHold;
		} else {
			f->pending = false;
		}

		if (want != f->waveHold) {
			this->handleButton(want, 1, true);
			f->waveHold = want;
			f->pending = false;
		}

		f->prevX = px;
		f->prevY = py;
		f->hasPrev = true;
	}

	// ================= КУБ =================
	// Планируем прыжки по физике куба на ~0.9 сек вперёд (можно несколько
	// прыжков подряд). Прыгаем в середине безопасного окна.
	void cubeLogic(float dt) {
		auto f = m_fields.self();

		if (f->holding) {
			f->releaseTimer -= dt;
			if (f->releaseTimer <= 0.f) {
				this->handleButton(false, 1, true);
				f->holding = false;
			}
			return;
		}
		if (!m_player1->m_isOnGround) {
			f->w0 = -1.f;
			return;
		}

		int legit = static_cast<int>(Mod::get()->getSettingValue<int64_t>("legit"));

		float px = m_player1->getPositionX();
		float py = m_player1->getPositionY();
		float s = m_player1->m_isUpsideDown ? -1.f : 1.f;
		float pyM = s * py;
		float vx = speedUnits(m_player1->m_playerSpeed) / 60.f; // юнитов за тик (1/60 c)

		constexpr float V0 = 11.180032f;
		constexpr float G = 0.958199f;
		constexpr float HS = 14.5f; // половина куба для блоков
		constexpr float HH = 15.5f; // для шипов (корпус куба 30x30)
		constexpr int TH = 64;      // горизонт в тиках
		constexpr int WMAX = 48;

		// препятствия впереди (в зеркальных координатах при перевёрнутой гравитации)
		f->window.clear();
		float reach = vx * TH + 40.f;
		auto it = std::lower_bound(f->obstacles.begin(), f->obstacles.end(),
			px - f->maxW - 20.f,
			[](Obstacle const& o, float v) { return o.x0 < v; });
		for (; it != f->obstacles.end() && it->x0 <= px + reach; ++it) {
			if (it->x1 < px - 20.f) continue;
			if (it->kind == K_SLOPE || it->kind == K_HSLOPE) continue;
			Obstacle m = *it;
			float a = s * it->y0, b = s * it->y1;
			m.y0 = std::min(a, b);
			m.y1 = std::max(a, b);
			if (m.y1 < pyM - 60.f || m.y0 > pyM + 110.f) continue;
			f->window.push_back(m);
		}
		if (f->window.empty()) {
			f->w0 = -1.f;
			return;
		}

		// 1) едем по земле: где первое столкновение?
		auto groundBad = [&](float x) {
			for (auto const& o : f->window) {
				bool haz = (o.kind == K_HAZARD);
				float rr = haz ? HH : HS;
				if (x > o.x0 - rr && x < o.x1 + rr && pyM > o.y0 - rr && pyM < o.y1 + rr) return true;
			}
			return false;
		};
		std::array<bool, TH + 1> runBad{};
		int firstBad = TH;
		for (int t = 0; t < TH; ++t) {
			runBad[t] = groundBad(px + vx * (t + 0.5f)) || groundBad(px + vx * (t + 1.f));
			if (runBad[t] && firstBad == TH) firstBad = t;
		}
		if (firstBad == TH) {
			f->w0 = -1.f; // прыгать не нужно
			return;
		}

		// 2) симуляция куба
		struct Sim { float x, y, vy; bool air; bool onBlock; };
		auto advance = [&](Sim& sm, float h) {
			float prevY = sm.y;
			if (sm.air) {
				sm.vy -= G * h;
				sm.y += sm.vy * h;
				if (sm.y <= pyM && sm.vy <= 0.f) {
					sm.y = pyM;
					sm.vy = 0.f;
					sm.air = false;
				}
			}
			sm.x += vx * h;
			for (auto const& o : f->window) {
				bool haz = (o.kind == K_HAZARD);
				float rr = haz ? HH : HS;
				if (!(sm.x > o.x0 - rr && sm.x < o.x1 + rr && sm.y > o.y0 - rr && sm.y < o.y1 + rr)) continue;
				if (haz) return false;
				if (sm.vy <= 0.f && prevY - HS >= o.y1 - 8.f) {        // приземлились на блок
					sm.y = o.y1 + HS;
					sm.vy = 0.f;
					sm.air = false;
					sm.onBlock = true;
				} else if (sm.vy > 0.f && prevY + HS <= o.y0 + 8.f) {  // стукнулись головой
					sm.y = o.y0 - HS;
					sm.vy = 0.f;
				} else {
					return false;                                      // врезались боком
				}
			}
			return true;
		};

		// good[t]: стоя на земле в тик t можно дожить до горизонта
		std::array<bool, TH + 1> good{};
		std::array<bool, TH + 1> jumpOk{};
		good[TH] = true;
		for (int t0 = TH - 1; t0 >= 0; --t0) {
			// прыжок в тик t0
			bool ok = false;
			Sim sm{px + vx * t0, pyM, V0, true, false};
			bool dead = false;
			bool done = false;
			for (float tm = static_cast<float>(t0); tm < TH && !done; tm += 0.5f) {
				if (!advance(sm, 0.5f)) { dead = true; break; }
				if (sm.onBlock) { ok = true; done = true; break; }
				if (!sm.air) {
					float tl = tm + 0.5f;
					int L = static_cast<int>(std::ceil(tl - 0.0001f));
					if (L >= TH) { ok = true; done = true; break; }
					bool partBad = (static_cast<float>(L) > tl + 0.0001f) && runBad[L - 1];
					ok = !partBad && good[L];
					done = true;
				}
			}
			if (!dead && !done) ok = true; // долетели до горизонта в воздухе
			jumpOk[t0] = ok;
			good[t0] = ok || (!runBad[t0] && good[t0 + 1]);
		}

		f->dbg.mode = 1;
		f->dbg.firstBad = firstBad;
		auto doJump = [&]() {
			f->dbg.jumped = true;
			f->dbg.jx = px;
			f->dbg.jy = py;
			this->handleButton(true, 1, true);
			f->holding = true;
			f->releaseTimer = 0.06f;
			f->w0 = -1.f;
			f->willMiss = false;
		};

		// 3) окно безопасных моментов прыжка [0..b]
		int b = -1;
		if (jumpOk[0]) {
			b = 0;
			while (b + 1 <= std::min(firstBad, WMAX) && jumpOk[b + 1]) ++b;
		}

		f->dbg.b = b;
		if (b < 0) {
			f->w0 = -1.f;
			bool later = false;
			for (int t = 1; t <= std::min(firstBad, WMAX) && !later; ++t) later = jumpOk[t];
			// выхода нет вообще -> аварийно прыгаем перед ближайшим препятствием
			if (!later && !good[0] && firstBad <= 10) {
				f->dbg.emergency = true;
				doJump();
			}
			return;
		}

		if (f->w0 < 0.f) {
			f->w0 = static_cast<float>(b);
			std::uniform_real_distribution<float> chance(0.f, 1.f);
			f->willMiss = chance(f->rng) < legit * 0.02f;
			if (legit > 0) {
				std::normal_distribution<float> noise(0.f, legit * 0.5f);
				f->jitter = noise(f->rng);
			} else {
				f->jitter = 0.f;
			}
		}
		if (f->willMiss) return;

		float threshold = f->w0 * 0.5f + f->jitter;
		f->dbg.w0 = f->w0;
		f->dbg.thr = threshold;
		if (static_cast<float>(b) <= threshold || b == 0) doJump();
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

		if (static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) return;
		if (!Mod::get()->getSettingValue<bool>("enabled")) return;
		if (!m_player1) return;

		auto f = m_fields.self();
		if (m_player1->m_isDead) {
			if (!f->deadLogged && f->built) {
				f->deadLogged = true;
				this->logDeath();
				f->dbg.jumped = false;
				f->dbg.emergency = false;
			}
			return;
		}
		f->deadLogged = false;
		if (!f->built) this->buildObjects();

		if (m_player1->m_isDart) {
			if (f->holding) {
				this->handleButton(false, 1, true);
				f->holding = false;
			}
			this->waveLogic(dt);
			return;
		}

		if (f->waveHold) {
			this->handleButton(false, 1, true);
			f->waveHold = false;
		}
		f->hasPrev = false;
		f->pending = false;

		bool cubeLike = !(m_player1->m_isShip || m_player1->m_isBird || m_player1->m_isBall ||
			m_player1->m_isRobot || m_player1->m_isSpider || m_player1->m_isSwing);
		if (cubeLike) this->cubeLogic(dt);
	}
};
