#pragma once
// Plugin BakkesMod : statistiques de la map « Aim training - By CoCo » (AimTrainingRL)
// Écrit chaque balle dans  %APPDATA%\bakkesmod\bakkesmod\data\aimcoco\tirs.jsonl

#include <filesystem>
#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "bakkesmod/wrappers/includes.h"
#include "bakkesmod/wrappers/kismet/SequenceWrapper.h"
#include "bakkesmod/wrappers/kismet/SequenceVariableWrapper.h"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

class AimCocoStats : public BakkesMod::Plugin::BakkesModPlugin
{
public:
    void onLoad() override;
    void onUnload() override;

private:
    struct Tir {
        bool actif = false;
        std::string date;
        std::string mode;
        std::string options;          // morceau de JSON avec les options de la map
        std::chrono::steady_clock::time_point debut;
        float duree = 0;              // s
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

    // Variable interne (Kismet) de la map, surveillée à chaque tick
    struct VarKismet {
        std::string nom;
        SequenceVariableWrapper var;
        bool estEntier;
        int valeur;
    };

    // hooks
    void onTick();
    void onTouche();
    void onButClassique();

    // Kismet
    void chargerKismet();
    void viderKismet();
    void surveillerKismet();
    int valeurKismet(const std::string& nom, int defaut = 0);
    std::string modeActuel();
    std::string optionsActuelles();

    // tirs
    bool surMapSuivie();
    void demarrerTir(const Vector& pos, const Vector& vel);
    void terminerTir();
    void marquerBut();
    void ecrireTir(const Tir& t);
    void ecrireLigne(const std::string& ligne, const std::string& fichier);
    void majHud();
    void dessiner(CanvasWrapper canvas);
    void dumpKismet();
    void modeSuivant();

    Tir tir;
    Tir enAttente;                    // tir terminé, écrit après un court délai (le score peut arriver juste après)
    int ticksAttente = 0;
    uintptr_t derniereBalle = 0;
    Vector dernierePos{0, 0, 0};

    std::vector<VarKismet> kismet;
    bool kismetCharge = false;
    bool scoreParKismet = false;      // la map compte les points elle-même (Streak / Count)

    // stats de la session en cours (pour l'affichage en jeu)
    int sessionTirs = 0;
    int sessionButs = 0;
    std::string dernierTirTexte;
    std::string derniereMap;

    std::mutex hudMutex;
    std::string hudLignes[3];
    bool hudVisible = false;
};
