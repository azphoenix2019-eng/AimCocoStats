#pragma once
// Plugin BakkesMod : statistiques de la map « Aim training - By CoCo »
// Écrit chaque tir dans  %APPDATA%\bakkesmod\bakkesmod\data\aimcoco\tirs.jsonl

#include <filesystem>
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/wrappers/includes.h"
#include "bakkesmod/wrappers/kismet/SequenceWrapper.h"
#include "bakkesmod/wrappers/kismet/SequenceVariableWrapper.h"

#include <chrono>
#include <mutex>
#include <string>

class AimCocoStats : public BakkesMod::Plugin::BakkesModPlugin
{
public:
    void onLoad() override;
    void onUnload() override;

private:
    struct Tir {
        bool actif = false;
        std::string date;
        std::chrono::steady_clock::time_point debut;
        Vector balleDepart{0, 0, 0};
        float vitesseDepart = 0;      // uu/s
        int ticks = 0;
        bool touche = false;
        int nbTouches = 0;
        int ticksDepuisTouche = 0;
        float tempsAvantTouche = 0;   // s
        Vector balleTouche{0, 0, 0};
        float vitesseTir = 0;         // uu/s, vitesse max juste après la 1re touche
        float hauteurVoiture = 0;     // uu
        bool but = false;
        int equipe = 0;
    };

    // hooks
    void onTick();
    void onTouche();
    void onBut();

    // outils
    bool surMapSuivie();
    void demarrerTir(const Vector& pos, const Vector& vel);
    void terminerTir();
    void ecrireLigne(const std::string& ligne, const std::string& fichier);
    void majHud();
    void dessiner(CanvasWrapper canvas);
    void dumpKismet();
    void modeSuivant();

    Tir tir;
    uintptr_t derniereBalle = 0;
    Vector dernierePos{0, 0, 0};

    // stats de la session en cours (pour l'affichage en jeu)
    int sessionTirs = 0;
    int sessionButs = 0;
    std::string dernierTirTexte;
    std::string derniereMap;

    std::mutex hudMutex;
    std::string hudLignes[3];
    bool hudVisible = false;
};
