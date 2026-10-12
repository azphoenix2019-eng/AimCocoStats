#include "pch.h"
#include "AimCocoStats.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

BAKKESMOD_PLUGIN(AimCocoStats, "Aim Coco Stats", "1.1", PLUGINTYPE_FREEPLAY)

namespace {
    constexpr float UU_EN_KMH = 0.036f;      // 1 uu/s = 0,036 km/h
    constexpr float UU_EN_M = 0.01f;         // 1 uu = 1 cm
    constexpr float SAUT_RESPAWN = 400.f;    // déplacement instantané = balle replacée
    constexpr int TICKS_MESURE_TIR = 15;     // ~0,12 s après la touche pour la vitesse du tir
    constexpr int TICKS_ATTENTE = 60;        // ~0,5 s après la fin d'une balle avant de l'écrire
    constexpr float BUT_Y = 5120.f, BUT_X = 893.f, BUT_Z = 642.f, RAYON_BALLE = 93.f;

    // Variables de la map qui augmentent quand une cible est touchée
    const char* VARS_SCORE[] = {"Streak", "Count"};

    std::string minuscules(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    std::string maintenant() {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::ostringstream o;
        o << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return o.str();
    }

    std::string json(const std::string& s) {
        std::string r = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') r += '\\';
            r += c;
        }
        return r + "\"";
    }

    std::string nb(float v, int decimales = 2) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(decimales) << v;
        return o.str();
    }

    float distance(const Vector& a, const Vector& b) {
        return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
    }
}

// ------------------------------------------------------------------ chargement

void AimCocoStats::onLoad()
{
    cvarManager->registerCvar("aimcoco_actif", "1", "Active l'enregistrement des tirs", true, true, 0, true, 1);
    cvarManager->registerCvar("aimcoco_map", "aimtraining", "Morceau du nom de la map à suivre (minuscules)");
    cvarManager->registerCvar("aimcoco_mode_auto", "1", "Lit le mode directement dans la map", true, true, 0, true, 1);
    cvarManager->registerCvar("aimcoco_mode", "Classique", "Mode utilisé si la détection automatique est coupée");
    cvarManager->registerCvar("aimcoco_modes", "Classique,Survie", "Liste des modes pour aimcoco_mode_suivant");
    cvarManager->registerCvar("aimcoco_hud", "1", "Affiche les stats en jeu", true, true, 0, true, 1);
    cvarManager->registerCvar("aimcoco_debug", "0", "Messages de debug dans la console F6", true, true, 0, true, 1);

    cvarManager->registerNotifier("aimcoco_mode_suivant", [this](std::vector<std::string>) { modeSuivant(); },
                                  "Passe au mode suivant (si la détection automatique est coupée)", PERMISSION_ALL);
    cvarManager->registerNotifier("aimcoco_definir_map", [this](std::vector<std::string>) {
        std::string map = minuscules(gameWrapper->GetCurrentMap());
        cvarManager->getCvar("aimcoco_map").setValue(map);
        cvarManager->log("[AimCoco] Map suivie : " + map);
    }, "Suit la map actuellement chargée", PERMISSION_ALL);
    cvarManager->registerNotifier("aimcoco_kismet", [this](std::vector<std::string>) { dumpKismet(); },
                                  "Écrit les variables Kismet de la map dans data/aimcoco/kismet.txt", PERMISSION_ALL);
    cvarManager->registerNotifier("aimcoco_map_actuelle", [this](std::vector<std::string>) {
        cvarManager->log("[AimCoco] Map actuelle : " + gameWrapper->GetCurrentMap());
    }, "Affiche le nom de la map chargée", PERMISSION_ALL);

    // tick physique (~120 fois par seconde)
    gameWrapper->HookEvent("Function TAGame.Car_TA.SetVehicleInput", [this](std::string) { onTick(); });
    gameWrapper->HookEvent("Function TAGame.Ball_TA.OnCarTouch", [this](std::string) { onTouche(); });
    gameWrapper->HookEvent("Function TAGame.Ball_TA.OnHitGoal", [this](std::string) { onButClassique(); });
    // sortie de la map : on clôt tout
    gameWrapper->HookEvent("Function TAGame.GameEvent_Soccar_TA.Destroyed", [this](std::string) {
        if (tir.actif) terminerTir();
        if (enAttente.actif) { ecrireTir(enAttente); enAttente.actif = false; }
        derniereBalle = 0;
        viderKismet();
    });

    gameWrapper->RegisterDrawable([this](CanvasWrapper c) { dessiner(c); });
    cvarManager->log("[AimCoco] Plugin chargé. Fichier : " +
                     (std::filesystem::path(gameWrapper->GetDataFolderW()) / "aimcoco" / "tirs.jsonl").string());
}

void AimCocoStats::onUnload()
{
    if (tir.actif) terminerTir();
    if (enAttente.actif) { ecrireTir(enAttente); enAttente.actif = false; }
    viderKismet();
}

// ------------------------------------------------------------------ Kismet (variables internes de la map)

void AimCocoStats::chargerKismet()
{
    kismetCharge = true;
    kismet.clear();
    scoreParKismet = false;
    SequenceWrapper sequence = gameWrapper->GetMainSequence();
    if (sequence.memory_address == 0) return;
    for (auto& [nom, var] : sequence.GetAllSequenceVariables(true)) {
        if (var.memory_address == 0) continue;
        if (var.IsInt()) kismet.push_back({nom, var, true, var.GetInt()});
        else if (var.IsBool()) kismet.push_back({nom, var, false, var.GetBool() ? 1 : 0});
        else continue;
        for (const char* s : VARS_SCORE)
            if (nom == s) scoreParKismet = true;
    }
    if (cvarManager->getCvar("aimcoco_debug").getBoolValue())
        cvarManager->log("[AimCoco] " + std::to_string(kismet.size()) + " variables surveillées, score par la map : " +
                         (scoreParKismet ? "oui" : "non"));
}

void AimCocoStats::viderKismet()
{
    kismet.clear();
    kismetCharge = false;
    scoreParKismet = false;
}

void AimCocoStats::surveillerKismet()
{
    bool debug = cvarManager->getCvar("aimcoco_debug").getBoolValue();
    for (VarKismet& k : kismet) {
        int v = k.estEntier ? k.var.GetInt() : (k.var.GetBool() ? 1 : 0);
        if (v == k.valeur) continue;
        if (debug)
            cvarManager->log("[AimCoco] " + k.nom + " : " + std::to_string(k.valeur) + " -> " + std::to_string(v));
        for (const char* s : VARS_SCORE)
            if (k.nom == s && v > k.valeur) marquerBut();
        k.valeur = v;
    }
}

int AimCocoStats::valeurKismet(const std::string& nom, int defaut)
{
    for (const VarKismet& k : kismet)
        if (k.nom == nom) return k.valeur;
    return defaut;
}

std::string AimCocoStats::modeActuel()
{
    if (!cvarManager->getCvar("aimcoco_mode_auto").getBoolValue() || kismet.empty())
        return cvarManager->getCvar("aimcoco_mode").getStringValue();
    std::string mode;
    if (valeurKismet("SurvivalMode")) mode = "Survie";
    else if (valeurKismet("ClassicMode")) mode = "Classique";
    else mode = "Autre";
    if (valeurKismet("EasyMode")) mode += " facile";
    return mode;
}

std::string AimCocoStats::optionsActuelles()
{
    auto b = [this](const char* nom) { return valeurKismet(nom) ? "true" : "false"; };
    return std::string(",\"cibles_mobiles\":") + b("MovingTargets") +
           ",\"cibles_aleatoires\":" + b("RandomTargets") +
           ",\"distances_aleatoires\":" + b("RandomDistances") +
           ",\"chutes_aleatoires\":" + b("RandomDrops");
}

// ------------------------------------------------------------------ détection

bool AimCocoStats::surMapSuivie()
{
    if (!cvarManager->getCvar("aimcoco_actif").getBoolValue()) return false;
    std::string cible = minuscules(cvarManager->getCvar("aimcoco_map").getStringValue());
    std::string map = minuscules(gameWrapper->GetCurrentMap());
    if (map != derniereMap) {          // nouvelle map : nouvelle session, nouvelles variables
        derniereMap = map;
        sessionTirs = sessionButs = 0;
        dernierTirTexte.clear();
        viderKismet();
    }
    return !cible.empty() && map.find(cible) != std::string::npos;
}

void AimCocoStats::onTick()
{
    if (!surMapSuivie()) {
        if (tir.actif) terminerTir();
        if (enAttente.actif) { ecrireTir(enAttente); enAttente.actif = false; }
        hudVisible = false;
        return;
    }
    if (!kismetCharge) chargerKismet();
    surveillerKismet();

    // le tir précédent est écrit une fois le délai passé
    if (enAttente.actif && ++ticksAttente >= TICKS_ATTENTE) {
        ecrireTir(enAttente);
        enAttente.actif = false;
    }

    ServerWrapper server = gameWrapper->GetGameEventAsServer();
    if (server.IsNull()) return;
    BallWrapper balle = server.GetBall();
    if (balle.IsNull()) {               // balle détruite (reset…)
        if (tir.actif) terminerTir();
        derniereBalle = 0;
        return;
    }

    Vector pos = balle.GetLocation();
    Vector vel = balle.GetVelocity();
    float vitesse = vel.magnitude();

    bool nouvelleBalle = balle.memory_address != derniereBalle;
    bool teleportee = derniereBalle != 0 && distance(pos, dernierePos) > SAUT_RESPAWN;
    derniereBalle = balle.memory_address;
    dernierePos = pos;

    if (nouvelleBalle || teleportee) {
        if (tir.actif) terminerTir();
        demarrerTir(pos, vel);
    }
    if (!tir.actif) return;

    tir.ticks++;
    if (!tir.touche && tir.ticks <= 15 && tir.vitesseDepart < 1.f)
        tir.vitesseDepart = vitesse;     // la map lance parfois la balle juste après l'apparition

    if (tir.touche && tir.ticksDepuisTouche < TICKS_MESURE_TIR) {
        tir.ticksDepuisTouche++;
        tir.vitesseTir = std::max(tir.vitesseTir, vitesse);
    }

    // maps sans score interne : on se rabat sur les buts classiques
    if (!scoreParKismet && !tir.but && std::fabs(pos.X) < BUT_X &&
        std::fabs(pos.Y) > BUT_Y + RAYON_BALLE && pos.Z < BUT_Z)
        marquerBut();

    majHud();
}

void AimCocoStats::onTouche()
{
    if (!tir.actif) return;
    ServerWrapper server = gameWrapper->GetGameEventAsServer();
    if (server.IsNull()) return;
    BallWrapper balle = server.GetBall();
    if (balle.IsNull()) return;

    tir.nbTouches++;
    if (tir.touche) return;
    tir.touche = true;
    tir.ticksDepuisTouche = 0;
    tir.tempsAvantTouche = std::chrono::duration<float>(std::chrono::steady_clock::now() - tir.debut).count();
    tir.balleTouche = balle.GetLocation();
    tir.vitesseTir = balle.GetVelocity().magnitude();

    CarWrapper voiture = gameWrapper->GetLocalCar();
    if (!voiture.IsNull()) {
        tir.hauteurVoiture = voiture.GetLocation().Z;
        PriWrapper pri = voiture.GetPRI();
        if (!pri.IsNull()) tir.equipe = pri.GetTeamNum();
    }
}

void AimCocoStats::onButClassique()
{
    if (!scoreParKismet) marquerBut();
}

void AimCocoStats::marquerBut()
{
    // si la map a déjà replacé la balle, le point revient au tir qui vient de se terminer
    Tir* cible = enAttente.actif ? &enAttente : (tir.actif ? &tir : nullptr);
    if (cible == nullptr || cible->but) return;
    cible->but = true;
    if (cvarManager->getCvar("aimcoco_debug").getBoolValue())
        cvarManager->log("[AimCoco] Cible touchée !");
}

// ------------------------------------------------------------------ tirs

void AimCocoStats::demarrerTir(const Vector& pos, const Vector& vel)
{
    tir = Tir{};
    tir.actif = true;
    tir.date = maintenant();
    tir.mode = modeActuel();
    tir.options = kismet.empty() ? "" : optionsActuelles();
    tir.debut = std::chrono::steady_clock::now();
    tir.balleDepart = pos;
    tir.vitesseDepart = vel.magnitude();
    if (cvarManager->getCvar("aimcoco_debug").getBoolValue())
        cvarManager->log("[AimCoco] Nouvelle balle (" + tir.mode + "), hauteur " + nb(pos.Z * UU_EN_M) + " m");
}

void AimCocoStats::terminerTir()
{
    tir.actif = false;
    tir.duree = std::chrono::duration<float>(std::chrono::steady_clock::now() - tir.debut).count();
    if (!tir.touche && !tir.but && tir.duree < 0.5f) return;   // reset immédiat : on ignore

    if (enAttente.actif) ecrireTir(enAttente);
    enAttente = tir;
    enAttente.actif = true;
    ticksAttente = 0;
}

void AimCocoStats::ecrireTir(const Tir& t)
{
    // but visé : celui de l'équipe adverse (bleu -> but orange en +Y)
    Vector cible{0.f, t.equipe == 0 ? BUT_Y : -BUT_Y, 321.f};

    std::ostringstream o;
    o << "{\"date\":" << json(t.date)
      << ",\"map\":" << json(gameWrapper->GetCurrentMap())
      << ",\"mode\":" << json(t.mode)
      << t.options
      << ",\"duree\":" << nb(t.duree)
      << ",\"touche\":" << (t.touche ? "true" : "false")
      << ",\"but\":" << (t.but ? "true" : "false")
      << ",\"nb_touches\":" << t.nbTouches
      << ",\"hauteur_depart\":" << nb(t.balleDepart.Z * UU_EN_M)
      << ",\"distance_depart\":" << nb(distance(t.balleDepart, cible) * UU_EN_M)
      << ",\"vitesse_depart\":" << nb(t.vitesseDepart * UU_EN_KMH, 1)
      << ",\"depart\":[" << nb(t.balleDepart.X, 0) << "," << nb(t.balleDepart.Y, 0) << "," << nb(t.balleDepart.Z, 0) << "]";
    if (t.touche) {
        o << ",\"temps_avant_touche\":" << nb(t.tempsAvantTouche)
          << ",\"hauteur_tir\":" << nb(t.balleTouche.Z * UU_EN_M)
          << ",\"distance_tir\":" << nb(distance(t.balleTouche, cible) * UU_EN_M)
          << ",\"vitesse_tir\":" << nb(t.vitesseTir * UU_EN_KMH, 1)
          << ",\"hauteur_voiture\":" << nb(t.hauteurVoiture * UU_EN_M);
    }
    o << "}";
    ecrireLigne(o.str(), "tirs.jsonl");

    if (t.touche) {
        sessionTirs++;
        if (t.but) sessionButs++;
        dernierTirTexte = std::string(t.but ? "Cible" : "Manque") + " - " + nb(t.vitesseTir * UU_EN_KMH, 0) +
                          " km/h, " + nb(distance(t.balleTouche, cible) * UU_EN_M, 0) + " m";
    }
    if (cvarManager->getCvar("aimcoco_debug").getBoolValue())
        cvarManager->log("[AimCoco] " + o.str());
}

void AimCocoStats::ecrireLigne(const std::string& ligne, const std::string& fichier)
{
    std::filesystem::path dossier = std::filesystem::path(gameWrapper->GetDataFolderW()) / "aimcoco";
    std::error_code ec;
    std::filesystem::create_directories(dossier, ec);
    std::ofstream f(dossier / fichier, std::ios::app);
    if (f) f << ligne << "\n";
    else cvarManager->log("[AimCoco] Impossible d'écrire dans " + (dossier / fichier).string());
}

void AimCocoStats::modeSuivant()
{
    std::string liste = cvarManager->getCvar("aimcoco_modes").getStringValue();
    std::string actuel = cvarManager->getCvar("aimcoco_mode").getStringValue();
    std::vector<std::string> modes;
    std::stringstream ss(liste);
    std::string m;
    while (std::getline(ss, m, ','))
        if (!m.empty()) modes.push_back(m);
    if (modes.empty()) return;
    auto it = std::find(modes.begin(), modes.end(), actuel);
    std::string suivant = (it == modes.end() || it + 1 == modes.end()) ? modes.front() : *(it + 1);
    cvarManager->getCvar("aimcoco_mode").setValue(suivant);
    cvarManager->log("[AimCoco] Mode : " + suivant);
}

// ------------------------------------------------------------------ affichage en jeu

void AimCocoStats::majHud()
{
    std::lock_guard<std::mutex> verrou(hudMutex);
    hudVisible = cvarManager->getCvar("aimcoco_hud").getBoolValue();
    hudLignes[0] = "Aim Coco - " + tir.mode;
    hudLignes[1] = "Session : " + std::to_string(sessionButs) + " / " + std::to_string(sessionTirs) + " cibles" +
                   (sessionTirs ? " (" + nb(100.f * sessionButs / sessionTirs, 0) + " %)" : "");
    hudLignes[2] = dernierTirTexte.empty() ? "" : "Dernier tir : " + dernierTirTexte;
}

void AimCocoStats::dessiner(CanvasWrapper canvas)
{
    std::lock_guard<std::mutex> verrou(hudMutex);
    if (!hudVisible) return;
    float y = 220.f;
    for (const std::string& ligne : hudLignes) {
        if (ligne.empty()) continue;
        canvas.SetColor((char)255, (char)255, (char)255, (char)230);
        canvas.SetPosition(Vector2F{20.f, y});
        canvas.DrawString(ligne, 1.3f, 1.3f, true);
        y += 22.f;
    }
}

// ------------------------------------------------------------------ exploration de la map

void AimCocoStats::dumpKismet()
{
    SequenceWrapper sequence = gameWrapper->GetMainSequence();
    if (sequence.memory_address == 0) {
        cvarManager->log("[AimCoco] Pas de séquence Kismet sur cette map.");
        return;
    }
    std::ostringstream o;
    o << "Map : " << gameWrapper->GetCurrentMap() << "\n";
    for (auto& [nom, var] : sequence.GetAllSequenceVariables(true)) {
        o << nom << " = ";
        if (var.IsInt()) o << "int " << var.GetInt();
        else if (var.IsFloat()) o << "float " << var.GetFloat();
        else if (var.IsBool()) o << "bool " << (var.GetBool() ? "true" : "false");
        else if (var.IsString()) o << "string " << var.GetString();
        else if (var.IsVector()) {
            Vector v = var.GetVector();
            o << "vector " << v.X << " " << v.Y << " " << v.Z;
        } else o << "(autre)";
        o << "\n";
    }
    std::filesystem::path dossier = std::filesystem::path(gameWrapper->GetDataFolderW()) / "aimcoco";
    std::error_code ec;
    std::filesystem::create_directories(dossier, ec);
    std::ofstream(dossier / "kismet.txt") << o.str();
    cvarManager->log(o.str());
    cvarManager->log("[AimCoco] Variables écrites dans " + (dossier / "kismet.txt").string());
}
