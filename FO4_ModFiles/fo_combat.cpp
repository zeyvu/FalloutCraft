// FalloutCraft: combat between Minecraft's player and Fallout's actors (port of the Skyrim
// plugin's Combat.cpp, the core of it).
//
//  - Fallout's actors near the player go to Minecraft every frame (actor table); Minecraft keeps
//    an invisible stand-in for each, which its swords, arrows, tridents and explosions can hit.
//  - Minecraft reports each hit (event ring): the Fallout actor takes the damage, scaled by its
//    level so Minecraft gear stays meaningful, staggers on knockback/critical hits, turns on the
//    player, and dies with the player credited.
//  - Fallout's hits on the player are taken back off Fallout's health and sent to Minecraft as
//    a share of the player's health (all of Fallout's health = Minecraft's 20), so Minecraft's
//    hearts are the player's health. Minecraft's death kills the Fallout player.

#include "fo_common.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace skycraft
{
	namespace
	{
		constexpr float kActorRange = 80.0f * static_cast<float>(proto::kUnitsPerBlock);  // stand-ins exist this far out
		constexpr float kHitMemorySeconds = 0.35f;  // how long a hit event can explain a health drop
		constexpr float kDotFlushSeconds = 1.0f;    // other damage (bleeding, radiation, fire...) is batched
		constexpr RE::TESFormID kPlayerRef = 0x14;

		// ---- what hit the player (main thread) -------------------------------------------------
		struct RecentHit
		{
			RE::TESFormID      attacker{ 0 };
			proto::HurtKind kind{ proto::kHurtMelee };
			std::uint32_t   flags{ 0 };
			float           age{ 99.0f };
		};
		RecentHit lastHit;

		struct PendingHurt
		{
			RE::TESFormID      attacker{ 0 };
			proto::HurtKind kind{ proto::kHurtOther };
			float           damage{ 0.0f };  // Minecraft-scaled (see SendHurt)
			float           age{ 0.0f };
		};
		PendingHurt dot;

		bool  essentialSet = false;
		bool  healthPrimed = false;
		float actorLogTimer = 0.0f;
		std::size_t lastActorCount = static_cast<std::size_t>(-1);

		RE::ActorValueInfo* HealthAV()
		{
			auto* avs = RE::ActorValue::GetSingleton();
			return avs ? avs->health : nullptr;
		}

		class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			static HitSink* Get()
			{
				static HitSink sink;
				return &sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player || a_event.target.get() != player || !State().puppeting) {
					return RE::BSEventNotifyControl::kContinue;
				}
				RecentHit hit;
				hit.attacker = a_event.cause ? a_event.cause->GetFormID() : 0;
				hit.age = 0.0f;
				hit.kind = a_event.projectileFormID != 0 ? proto::kHurtProjectile : proto::kHurtMelee;
				if (a_event.usesHitData && a_event.hitData.flags.any(RE::HitData::Flag::kPowerAttack)) {
					hit.flags |= proto::kHurtPowerAttack;
				}
				if (a_event.usesHitData && a_event.hitData.flags.any(RE::HitData::Flag::kBlocked)) {
					hit.flags |= proto::kHurtBlockedInSkyrim;
				}
				lastHit = hit;
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Minecraft's side divides by 5 (it was written for Skyrim's health); this sends the hit as
		// the same share of Minecraft's 20 health as it took of Fallout's.
		float ToMinecraftShare(float a_damage, float a_maxHealth)
		{
			return a_maxHealth > 1.0f ? a_damage * 100.0f / a_maxHealth : a_damage;
		}

		void SendHurt(proto::HurtKind a_kind, float a_share, RE::TESFormID a_attacker, std::uint32_t a_flags, float a_falloutDamage)
		{
			if (a_share <= 0.01f) {
				return;
			}
			link::PushInput(proto::kInHurt, static_cast<std::uint16_t>(a_kind), static_cast<std::int32_t>(a_share * 100.0f),
				static_cast<std::int32_t>(a_attacker), static_cast<std::int32_t>(a_flags));
			REX::INFO("player hit by {:08X}: {:.1f} Fallout damage ({}) -> {:.1f} Minecraft health", a_attacker, a_falloutDamage,
				a_kind == proto::kHurtMelee ? "melee" : a_kind == proto::kHurtProjectile ? "projectile" : a_kind == proto::kHurtMagic ? "magic" : "other",
				a_share / 5.0f);
		}

		void SetEssential(RE::PlayerCharacter* a_player, bool a_on)
		{
			if (a_on == essentialSet) {
				return;
			}
			if (a_on) {
				a_player->boolFlags.set(RE::Actor::BOOL_FLAGS::kEssential);
			} else {
				a_player->boolFlags.reset(RE::Actor::BOOL_FLAGS::kEssential);
			}
			essentialSet = a_on;
		}

		// Fallout's damage on the player's health so far that has already been handled (refunded
		// or, if Fallout wouldn't take the refund, at least never sent twice).
		float baseline = 0.0f;
		float sinceHurt = 99.0f;  // seconds since a hurt was sent to Minecraft
		constexpr float kHurtSpacing = 0.55f;  // Minecraft's invulnerability after a hurt is 0.5 s

		struct QueuedHit
		{
			proto::HurtKind kind{ proto::kHurtMelee };
			RE::TESFormID   attacker{ 0 };
			std::uint32_t   flags{ 0 };
			float           share{ 0.0f };
			float           fallout{ 0.0f };
		};
		QueuedHit queued;
		int   refundLogs = 0;

		float Deficit(RE::PlayerCharacter* a_player, RE::ActorValueInfo& a_health)
		{
			return std::max(0.0f, -a_player->GetModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, a_health));
		}

		// Gives the player's health back; returns what is still missing afterwards.
		float Refund(RE::PlayerCharacter* a_player, RE::ActorValueInfo& a_health, float a_deficit)
		{
			a_player->RestoreActorValue(a_health, a_deficit);
			float left = Deficit(a_player, a_health);
			if (left > 0.01f) {
				a_player->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, a_health, left);
				const float after = Deficit(a_player, a_health);
				if (refundLogs < 5) {
					++refundLogs;
					REX::INFO("combat: refunding {:.2f} health: restore left {:.2f}, damage modifier left {:.2f}", a_deficit, left, after);
				}
				left = after;
			}
			return left;
		}

		// Fallout's health bar shows Minecraft's hearts: the player's health is set to the same share
		// of its maximum (never below 1: Minecraft decides when the player dies). Without Minecraft's
		// health (an older mod), Fallout's damage is just given back. Returns the deficit left.
		float Mirror(RE::PlayerCharacter* a_player, RE::ActorValueInfo& a_health, float a_max, float a_deficit)
		{
			const float frac = State().mcHealth;
			if (frac < 0.0f) {
				return a_deficit > 0.01f ? Refund(a_player, a_health, a_deficit) : a_deficit;
			}
			const float target = std::clamp(a_max * (1.0f - frac), 0.0f, std::max(a_max - 1.0f, 0.0f));
			const float delta = a_deficit - target;  // > 0: heal Fallout's player, < 0: hurt them
			if (delta > 0.05f) {
				a_player->RestoreActorValue(a_health, delta);
				const float left = Deficit(a_player, a_health) - target;
				if (left > 0.05f) {
					a_player->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, a_health, left);
				}
			} else if (delta < -0.05f) {
				a_player->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, a_health, delta);
			}
			return Deficit(a_player, a_health);
		}

		struct Burst
		{
			float share{ 0.0f };
			float fallout{ 0.0f };
			float age{ 0.0f };
		};
		Burst burst;

		void AddToQueue(proto::HurtKind a_kind, RE::TESFormID a_attacker, std::uint32_t a_flags, float a_share, float a_fallout)
		{
			// A melee or projectile hit names the attacker: Minecraft's armour and shield then apply.
			if (queued.share <= 0.0f || queued.kind == proto::kHurtMagic || queued.kind == proto::kHurtOther) {
				queued.kind = a_kind;
				queued.attacker = a_attacker;
			}
			queued.flags |= a_flags;
			queued.share += a_share;
			queued.fallout += a_fallout;
		}

		// The closest living actor fighting (hostile to) the player, within the actor table's range.
		RE::TESFormID NearestFoe(RE::PlayerCharacter* a_player)
		{
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return 0;
			}
			const auto    playerPos = a_player->GetPosition();
			float         best = kActorRange;
			RE::TESFormID id = 0;
			for (auto& handle : lists->highActorHandles) {
				auto  ptr = handle.get();
				auto* actor = ptr.get();
				if (!actor || actor == a_player || actor->IsDead(false) || !actor->IsInCombat() || !actor->GetHostileToActor(a_player)) {
					continue;
				}
				const float d = actor->GetPosition().GetDistance(playerPos);
				if (d < best) {
					best = d;
					id = actor->GetFormID();
				}
			}
			return id;
		}

		// Minecraft owns the player's health: Fallout's damage is refunded here and sent to Minecraft.
		void BridgePlayerDamage(RE::PlayerCharacter* a_player, float a_delta)
		{
			auto* health = HealthAV();
			if (!health) {
				return;
			}
			const float max = a_player->GetPermanentActorValue(*health);
			const float deficit = Deficit(a_player, *health);
			lastHit.age += a_delta;
			if (!healthPrimed) {
				// Whatever damage the save had before Minecraft took over isn't a new hit.
				healthPrimed = true;
				baseline = Mirror(a_player, *health, max, deficit);
				return;
			}
			const float fresh = deficit - baseline;
			sinceHurt += a_delta;
			if (fresh > 0.01f) {
				const float share = ToMinecraftShare(fresh, max);
				if (lastHit.age < kHitMemorySeconds) {
					AddToQueue(lastHit.kind, lastHit.attacker, lastHit.flags, share + dot.damage, fresh);
					lastHit.age = 99.0f;  // one hit event explains one health drop
					dot = PendingHurt{};
				} else if (fresh >= max * 0.02f) {
					// A chunk at once is a hit, not bleeding. Fallout often reports gunshots after the
					// health has already dropped: wait a moment for the hit event that names the shooter.
					burst.share += share;
					burst.fallout += fresh;
				} else {
					dot.kind = proto::kHurtMagic;  // bleeding, poison, radiation, fire: through armour, like Minecraft's
					dot.damage += share;
				}
			}
			if (burst.share > 0.0f) {
				burst.age += a_delta;
				if (lastHit.age < kHitMemorySeconds) {
					AddToQueue(lastHit.kind, lastHit.attacker, lastHit.flags, burst.share, burst.fallout);
					lastHit.age = 99.0f;
					burst = Burst{};
				} else if (burst.age > 0.3f) {
					// No event came: most likely a shot from whoever is fighting the player nearest.
					const RE::TESFormID shooter = NearestFoe(a_player);
					AddToQueue(shooter ? proto::kHurtProjectile : proto::kHurtMagic, shooter, 0, burst.share, burst.fallout);
					REX::INFO("combat: {:.1f} Fallout damage without a hit event; sent as {} from {:08X}", burst.fallout, shooter ? "a shot" : "magic", shooter);
					burst = Burst{};
				}
			}
			baseline = Mirror(a_player, *health, max, deficit);
			// Minecraft ignores most of a hurt that lands within half a second of the last one
			// (its invulnerability), so hits that come close together are sent together.
			if (queued.share > 0.0f && sinceHurt >= kHurtSpacing) {
				SendHurt(queued.kind, queued.share, queued.attacker, queued.flags, queued.fallout);
				queued = QueuedHit{};
				sinceHurt = 0.0f;
			}
			dot.age += a_delta;
			if (dot.age >= kDotFlushSeconds && sinceHurt >= kHurtSpacing && dot.damage > 0.0f && queued.share <= 0.0f) {
				SendHurt(dot.kind, dot.damage, dot.attacker, 0, dot.damage * max / 100.0f);
				dot = PendingHurt{};
				sinceHurt = 0.0f;
			} else if (dot.damage <= 0.0f) {
				dot.age = 0.0f;
			}
		}

		void WriteActorTable(RE::PlayerCharacter* a_player)
		{
			static std::vector<proto::ActorRecord> records;
			records.clear();
			auto* lists = RE::ProcessLists::GetSingleton();
			auto* health = HealthAV();
			if (!lists || !health) {
				link::WriteActors(nullptr, 0);
				return;
			}
			const auto playerPos = a_player->GetPosition();
			const float k = 1.0f / static_cast<float>(proto::kUnitsPerBlock);
			for (auto& handle : lists->highActorHandles) {
				auto  actorPtr = handle.get();
				auto* actor = actorPtr.get();
				if (!actor || actor == a_player || actor->IsDisabled() || actor->IsDeleted()) {
					continue;
				}
				auto* root = actor->Get3D();
				if (!root) {
					continue;
				}
				const auto pos = actor->GetPosition();
				if (pos.GetDistance(playerPos) > kActorRange) {
					continue;
				}
				proto::ActorRecord r{};
				r.formId = actor->GetFormID();
				const bool dead = actor->IsDead(false);
				r.flags = (actor->GetHostileToActor(a_player) ? proto::kActorHostile : 0u) | (dead ? proto::kActorDead : 0u) |
				          (actor->boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential) ? proto::kActorEssential : 0u) |
				          (actor->IsInCombat() ? proto::kActorInCombat : 0u);
				const auto mc = GameToMc(pos);
				r.x = static_cast<float>(mc.x);
				r.y = static_cast<float>(mc.y);
				r.z = static_cast<float>(mc.z);
				r.yaw = HeadingToMcYaw(actor->data.angle.z);
				// The 3D's bounding sphere: about the actor's height across for people, wider for
				// mirelurks, deathclaws and the like.
				const float radius = std::clamp(root->worldBound.fRadius, 20.0f, 600.0f);
				r.height = std::clamp(radius * 1.8f * k, 0.5f, 10.0f);
				r.width = std::clamp(radius * 0.8f * k, 0.4f, 6.0f);
				const float maxHealth = actor->GetPermanentActorValue(*health);
				r.healthFrac = maxHealth > 0.0f ? std::clamp(actor->GetActorValue(*health) / maxHealth, 0.0f, 1.0f) : 0.0f;
				r.level = static_cast<std::uint16_t>(std::max<std::int16_t>(actor->GetLevel(), 0));
				if (const char* name = actor->GetDisplayFullName()) {
					strncpy_s(r.name, name, _TRUNCATE);
				}
				records.push_back(r);
				if (records.size() >= proto::kMaxActors) {
					break;
				}
			}
			link::WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
			if (records.size() != lastActorCount && actorLogTimer <= 0.0f) {
				lastActorCount = records.size();
				actorLogTimer = 5.0f;
				std::string names;
				for (std::size_t i = 0; i < records.size() && i < 8; ++i) {
					names += std::format(" '{}'{}", records[i].name, (records[i].flags & proto::kActorHostile) ? "(hostile)" : "");
				}
				REX::INFO("combat: {} Fallout actors mirrored in Minecraft:{}", records.size(), names);
			}
		}

		// Fallout's console commands run on a reference (StartCombat): the same thing the console
		// does, through one reused script form.
		void RunCommand(RE::TESObjectREFR* a_on, const std::string& a_command)
		{
			static RE::Script* script = [] {
				auto  factories = RE::IFormFactory::GetFormFactories();
				auto* factory = factories[std::to_underlying(RE::ENUM_FORM_ID::kSCPT)];
				return factory ? static_cast<RE::Script*>(factory->DoCreate()) : nullptr;
			}();
			if (!script || !a_on) {
				return;
			}
			script->SetText(a_command);
			RE::ScriptCompiler compiler;
			script->CompileAndRun(&compiler, RE::COMPILER_NAME::kSystemWindow, a_on);
		}

		// A Minecraft hit on an actor's stand-in.
		void ApplyHit(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			auto* actor = RE::TESForm::GetFormByID<RE::Actor>(a_ev.formId);
			auto* health = HealthAV();
			if (!actor || !health || actor->IsDead(false)) {
				return;
			}
			const float level = static_cast<float>(std::max<std::int16_t>(actor->GetLevel(), 1));
			const float damage = a_ev.a * (5.0f + 0.25f * level);
			const bool  crit = (a_ev.flags & proto::kHitCritical) != 0;
			const bool  projectile = (a_ev.flags & proto::kHitProjectile) != 0;
			const float push = a_ev.d;
			if (damage <= 0.0f) {
				return;
			}
			const float before = actor->GetActorValue(*health);
			actor->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *health, -damage);
			const float after = actor->GetActorValue(*health);
			const bool  essential = actor->boolFlags.all(RE::Actor::BOOL_FLAGS::kEssential);
			bool        killed = false;
			if (after <= 0.0f && !essential && !actor->IsDead(false)) {
				actor->KillImpl(a_player, damage, true, false);
				killed = true;
			}
			if (!killed) {
				// Sprint hits, knockback and crits stagger; ordinary swings don't (Minecraft's attack
				// rate would otherwise stun-lock everything).
				if (push > 0.45f || crit) {
					const float magnitude = std::clamp(crit ? 0.5f : (push - 0.4f) * 1.5f, 0.25f, 1.0f);
					actor->SetGraphVariableFloat("staggerMagnitude", magnitude);
					actor->NotifyAnimationGraphImpl("staggerStart");
				}
				if (!actor->IsInCombat()) {
					RunCommand(actor, "StartCombat 14");
				}
			}
			REX::INFO("hit '{}' ({:08X}, level {:.0f}) for {:.1f} Minecraft -> {:.0f} Fallout damage, health {:.0f} -> {:.0f}{}{}{}{}",
				actor->GetDisplayFullName() ? actor->GetDisplayFullName() : "", a_ev.formId, level, a_ev.a, damage, before, after, crit ? ", critical" : "",
				projectile ? ", projectile" : "", push > 0.45f ? ", knockback" : "", killed ? ", killed" : "");
		}

		void KillPlayer(RE::PlayerCharacter* a_player, const proto::McEvent& a_ev)
		{
			if (a_player->IsDead(false)) {
				return;
			}
			REX::INFO("Minecraft player died (killer {:08X}); killing the Fallout player", a_ev.formId);
			SetEssential(a_player, false);
			auto* killer = a_ev.formId ? RE::TESForm::GetFormByID<RE::Actor>(a_ev.formId) : nullptr;
			auto* health = HealthAV();
			const float current = health ? a_player->GetActorValue(*health) : 1000.0f;
			if (health) {
				a_player->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *health, -(current + 1.0f));
			}
			a_player->KillImpl(killer, current + 1.0f, true, false);
		}
	}

	namespace Combat
	{
		void Install()
		{
			if (auto* source = RE::TESHitEvent::GetEventSource()) {
				source->RegisterSink(HitSink::Get());
				REX::INFO("combat: listening to Fallout's hit events");
			} else {
				REX::WARN("combat: no hit event source; hits on the player won't name the attacker");
			}
		}

		void PerFrame(RE::PlayerCharacter* a_player, bool a_puppeting, float a_delta)
		{
			actorLogTimer -= a_delta;
			if (!a_puppeting) {
				if (essentialSet) {
					SetEssential(a_player, false);
				}
				healthPrimed = false;
				queued = QueuedHit{};
				burst = Burst{};
				dot = PendingHurt{};
				// Still drain Minecraft's events so stale hits don't land when control resumes.
				proto::McEvent ev;
				while (link::PopEvent(ev)) {
					if (ev.type == proto::kEvPlayerDied && link::MinecraftAlive()) {
						KillPlayer(a_player, ev);
					}
				}
				link::WriteActors(nullptr, 0);
				return;
			}
			// Fallout can't kill the player while Minecraft decides (a big hit leaves them on their
			// feet); Minecraft's health does the rest.
			SetEssential(a_player, true);
			WriteActorTable(a_player);

			proto::McEvent ev;
			int            budget = 64;
			while (budget-- > 0 && link::PopEvent(ev)) {
				switch (ev.type) {
				case proto::kEvHitActor:
					ApplyHit(a_player, ev);
					break;
				case proto::kEvPlayerDied:
					KillPlayer(a_player, ev);
					break;
				case proto::kEvExplosion:
					REX::INFO("Minecraft explosion at ({:.1f}, {:.1f}, {:.1f}), radius {:.1f} (its damage on actors arrives as hits)", ev.a, ev.b, ev.c, ev.d);
					break;
				default:
					break;
				}
			}
			if (!a_player->IsDead(false)) {
				BridgePlayerDamage(a_player, a_delta);
			}
		}
	}
}
