#include <string>
#include <vector>
#include <utility>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <stdexcept>
#include <unordered_set>
#include <algorithm>
#include <random>
#include <queue>
#include <set>
#include <cmath>
#include <cassert>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include "HillClimbing.hpp"
#include "../measures/localMeasures/LocalMeasure.hpp"
#include "../measures/localMeasures/GenericLocalMeasure.hpp"
#include "../measures/WeightedEdgeConservation.hpp"
#include "../measures/Measure.hpp"
#include "../utils/Timer.hpp"
#include "../utils/randomSeed.hpp"

using namespace std;

HillClimbing::HillClimbing(Graph* G1, Graph* G2, MeasureCombination* M, string startAName):
    Method(G1, G2, "hillclimbing"), M(M), startAName(startAName), startA(Alignment::empty())
{

    uint n1 = G1->getNumNodes();
    uint n2 = G2->getNumNodes();

    uint ramificationChange = n1*(n2-n1);
    uint ramificationSwap = n1*(n1-1)/2;
    uint totalRamification = ramificationSwap + ramificationChange;
    changeProbability = (double) ramificationChange/totalRamification;

    if (startAName == "") {
        startA = Alignment::random(n1, n2);
    }
    else {
        startA = Alignment::loadMapping(startAName);
    }

    random_device rd;
    gen = mt19937(getRandomSeed());
    G1RandomNode = uniform_int_distribution<>(0, n1-1);
    G2RandomUnassignedNode = uniform_int_distribution<>(0, n2-n1-1);
    randomReal = uniform_real_distribution<>(0, 1);

    executionTime = 0;
}

HillClimbing::~HillClimbing() {
}

double HillClimbing::getExecutionTime() const {
    return executionTime;
}

Alignment HillClimbing::run() {
    int numSame = 0;
    uint n1 = G1->getNumNodes();
    uint n2 = G2->getNumNodes();

    vector<ushort> A(startA.getMapping());
#ifdef WEIGHTED
    vector<vector<ushort> > G1AdjMatrix;
    vector<vector<ushort> > G2AdjMatrix;
#else
    vector<vector<bool> > G1AdjMatrix;
    vector<vector<bool> > G2AdjMatrix;
#endif

    G1->getAdjMatrix(G1AdjMatrix);
    G2->getAdjMatrix(G2AdjMatrix);
    vector<vector<ushort> > G1AdjLists;
    G1->getAdjLists(G1AdjLists);
    vector<vector<ushort> > G2AdjLists;
    G2->getAdjLists(G2AdjLists);

    vector<bool> assignedNodesG2(n2, false);
    for (uint i = 0; i < n1; i++) {
        assignedNodesG2[A[i]] = true;
    }
    vector<ushort> unassignedNodesG2(n2-n1);
    int j = 0;
    for (uint i = 0; i < n2; i++) {
        if (not assignedNodesG2[i]) {
            unassignedNodesG2[j] = i;
            j++;
        }
    }

    //initialize data structures for incremental evaluation of ec/ics/s3
    double ecWeight = M->getWeight("ec");
    double icsWeight = M->getWeight("ics");
    double s3Weight = M->getWeight("s3");
    double g1Edges = G1->getNumEdges();
    double g1Nodes = n1;

    int aligEdges = Alignment(A).numAlignedEdges(*G1, *G2);
    bool needG2InducedEdges = (icsWeight > 0 or s3Weight > 0);
    int g2InducedEdges;
    if (needG2InducedEdges) g2InducedEdges = G2->numNodeInducedSubgraphEdges(A);
    else g2InducedEdges = 1; //dummy value

    //initialize data structures for incremental evaluation of local measures
    vector<vector<float> > localsCombined (n1, vector<float> (n2, 0));
    for (uint i = 0; i < M->numMeasures(); i++) {
        Measure* m = M->getMeasure(i);
        float weight = M->getWeight(m->getName());
        if (m->isLocal() and weight > 0) {
            vector<vector<float> >* simMatrix = ((LocalMeasure*) m)->getSimMatrix();
            for (uint i = 0; i < n1; i++) {
                for (uint j = 0; j < n2; j++) {
                    localsCombined[i][j] += weight * (*simMatrix)[i][j];
                }
            }
        }
    }
    Measure* allLocals = new GenericLocalMeasure(G1, G2, "locals", localsCombined);
    double localScoreSum = allLocals->eval(Alignment(A)) * n1;

    //initialize data structures for incremental evaluation of WEC
    double wecWeight = M->getWeight("wec");
    double wecSum = 0;
    vector<vector<float> >* wecSimMatrix = NULL;
    if (wecWeight > 0) {
        WeightedEdgeConservation* wec = (WeightedEdgeConservation*) M->getMeasure("wec");
        wecSum = wec->eval(Alignment(A))*2*g1Edges;
        wecSimMatrix = wec->getNodeSimMeasure()->getSimMatrix();
    }
    double currentScore = M->eval(Alignment(A));

    Timer timer;
    timer.start();
    for (long long unsigned int i = 0; ; i++) {
        if (i%100 == 0) {
            cout << timer.elapsedString() << " " << currentScore << endl;
        }
        double bestNewCurrentScore = currentScore;

        //dummy initializations:
        bool useChangeOperator = false;
        ushort bestSource = 0;
        uint bestNewTargetIndex = 0;
        int bestNewAligEdges = 0;
        int bestNewG2InducedEdges = 0;
        double bestNewLocalScoreSum = 0;
        double bestNewWecSum = 0;
        ushort bestSource1 = 0, bestSource2 = 0;

        //traverse all change neighbors
        for (ushort source = 0; source < n1; source++) {
            for (uint newTargetIndex = 0; newTargetIndex < n2-n1; newTargetIndex++) {
                ushort oldTarget = A[source];
                ushort newTarget = unassignedNodesG2[newTargetIndex];

                int newAligEdges = aligEdges;
                for (uint j = 0; j < G1AdjLists[source].size(); j++) {
                    ushort neighbor = G1AdjLists[source][j];
                    newAligEdges -= G2AdjMatrix[oldTarget][A[neighbor]];
                    newAligEdges += G2AdjMatrix[newTarget][A[neighbor]];
                }

                int newG2InducedEdges = g2InducedEdges;
                if (needG2InducedEdges) {
                    for (uint j = 0; j < G2AdjLists[oldTarget].size(); j++) {
                        ushort neighbor = G2AdjLists[oldTarget][j];
                        newG2InducedEdges -= assignedNodesG2[neighbor];
                    }
                    for (uint j = 0; j < G2AdjLists[newTarget].size(); j++) {
                        ushort neighbor = G2AdjLists[newTarget][j];
                        newG2InducedEdges += assignedNodesG2[neighbor];
                    }
                    //address case changing between adjacent nodes:
                    newG2InducedEdges -= G2AdjMatrix[oldTarget][newTarget];
                }

                double newLocalScoreSum = localScoreSum +
                    localsCombined[source][newTarget] -
                    localsCombined[source][oldTarget];

                double newWecSum = wecSum;
                if (wecWeight > 0) {
                    for (uint j = 0; j < G1AdjLists[source].size(); j++) {
                        ushort neighbor = G1AdjLists[source][j];
                        if (G2AdjMatrix[oldTarget][A[neighbor]]) {
                            newWecSum -= (*wecSimMatrix)[source][oldTarget];
                            newWecSum -= (*wecSimMatrix)[neighbor][A[neighbor]];
                        }
                        if (G2AdjMatrix[newTarget][A[neighbor]]) {
                            newWecSum += (*wecSimMatrix)[source][newTarget];
                            newWecSum += (*wecSimMatrix)[neighbor][A[neighbor]];
                        }
                    }
                }

                double newCurrentScore = newLocalScoreSum / g1Nodes +
                    newAligEdges*(ecWeight/g1Edges +
                                  icsWeight/newG2InducedEdges +
                                  s3Weight/(g1Edges + newG2InducedEdges - newAligEdges)) +
                    wecWeight*(newWecSum/(2*g1Edges));

                if (newCurrentScore > bestNewCurrentScore) {
                    bestNewCurrentScore = newCurrentScore;
                    useChangeOperator = true;
                    bestSource = source;
                    bestNewTargetIndex = newTargetIndex;
                    bestNewAligEdges = newAligEdges;
                    bestNewG2InducedEdges = newG2InducedEdges;
                    bestNewLocalScoreSum = newLocalScoreSum;
                    bestNewWecSum = newWecSum;
                }
            }
        }

        //traverse all swap neighbors
        for (ushort source1 = 0; source1 < n1; source1++) {
            for (ushort source2 = source1+1; source2 < n1; source2++) {
                ushort target1 = A[source1], target2 = A[source2];

                int newAligEdges = aligEdges;
                for (uint j = 0; j < G1AdjLists[source1].size(); j++) {
                    ushort neighbor = G1AdjLists[source1][j];
                    newAligEdges -= G2AdjMatrix[target1][A[neighbor]];
                    newAligEdges += G2AdjMatrix[target2][A[neighbor]];
                }
                for (uint j = 0; j < G1AdjLists[source2].size(); j++) {
                    ushort neighbor = G1AdjLists[source2][j];
                    newAligEdges -= G2AdjMatrix[target2][A[neighbor]];
                    newAligEdges += G2AdjMatrix[target1][A[neighbor]];
                }
                //address case swapping between adjacent nodes with adjacent images:
#ifdef WEIGHTED
                newAligEdges += (-1 << 1) & (G1AdjMatrix[source1][source2] + G2AdjMatrix[target1][target2]);
#else
                newAligEdges += 2*(G1AdjMatrix[source1][source2] & G2AdjMatrix[target1][target2]);
#endif
                double newLocalScoreSum = localScoreSum +
                    localsCombined[source1][target2] -
                    localsCombined[source1][target1] +
                    localsCombined[source2][target1] -
                    localsCombined[source2][target2];

                double newWecSum = wecSum;
                if (wecWeight > 0) {
                    for (uint j = 0; j < G1AdjLists[source1].size(); j++) {
                        ushort neighbor = G1AdjLists[source1][j];
                        if (G2AdjMatrix[target1][A[neighbor]]) {
                            newWecSum -= (*wecSimMatrix)[source1][target1];
                            newWecSum -= (*wecSimMatrix)[neighbor][A[neighbor]];
                        }
                        if (G2AdjMatrix[target2][A[neighbor]]) {
                            newWecSum += (*wecSimMatrix)[source1][target2];
                            newWecSum += (*wecSimMatrix)[neighbor][A[neighbor]];
                        }
                    }
                    for (uint j = 0; j < G1AdjLists[source2].size(); j++) {
                        ushort neighbor = G1AdjLists[source2][j];
                        if (G2AdjMatrix[target2][A[neighbor]]) {
                            newWecSum -= (*wecSimMatrix)[source2][target2];
                            newWecSum -= (*wecSimMatrix)[neighbor][A[neighbor]];
                        }
                        if (G2AdjMatrix[target1][A[neighbor]]) {
                            newWecSum += (*wecSimMatrix)[source2][target1];
                            newWecSum += (*wecSimMatrix)[neighbor][A[neighbor]];
                        }
                    }
                    //address case swapping between adjacent nodes with adjacent images:
#ifdef WEIGHTED
                    if (G1AdjMatrix[source1][source2] > 0 and G2AdjMatrix[target1][target2] > 0) { 
#else
                    if (G1AdjMatrix[source1][source2] and G2AdjMatrix[target1][target2]) {
#endif
                        //not sure about this -- but seems fine
                        newWecSum += 2*(*wecSimMatrix)[source1][target1];
                        newWecSum += 2*(*wecSimMatrix)[source2][target2];
                    }
                }

                double newCurrentScore = newLocalScoreSum / g1Nodes +
                    newAligEdges*(ecWeight/g1Edges +
                                  icsWeight/g2InducedEdges +
                                  s3Weight/(g1Edges + g2InducedEdges - newAligEdges)) +
                    wecWeight * (newWecSum/(2*g1Edges));

                if (newCurrentScore > bestNewCurrentScore) {
                    bestNewCurrentScore = newCurrentScore;
                    useChangeOperator = false;
                    bestSource1 = source1;
                    bestSource2 = source2;
                    bestNewAligEdges = newAligEdges;
                    bestNewLocalScoreSum = newLocalScoreSum;
                    bestNewWecSum = newWecSum;
                }
            }
        }

        if (bestNewCurrentScore == currentScore && ++numSame >= 10 ) {
            executionTime = timer.elapsed();
            return A;
        }
    numSame = 0;

        currentScore = bestNewCurrentScore;
        aligEdges = bestNewAligEdges;
        localScoreSum = bestNewLocalScoreSum;
        wecSum = bestNewWecSum;

        if (useChangeOperator) {
            ushort newTarget = unassignedNodesG2[bestNewTargetIndex];
            ushort oldTarget = A[bestSource];
            A[bestSource] = newTarget;
            unassignedNodesG2[bestNewTargetIndex] = oldTarget;
            assignedNodesG2[oldTarget] = false;
            assignedNodesG2[newTarget] = true;
            g2InducedEdges = bestNewG2InducedEdges;
        }
        else {
            ushort target1 = A[bestSource1], target2 = A[bestSource2];
            A[bestSource1] = target2;
            A[bestSource2] = target1;
        }
    }
}

string HillClimbing::fileNameSuffix(const Alignment& A) {
    return "_" + M->toString() + "_" + extractDecimals(M->eval(A),3);
}

void HillClimbing::describeParameters(ostream& stream) {
    stream << "starting alignment: ";
    if (startAName == "") stream << "'random'" << endl;
    else stream << startAName << endl;
    stream << endl << "Optimization criteria:" << endl;
    M->printWeights(stream);
}
