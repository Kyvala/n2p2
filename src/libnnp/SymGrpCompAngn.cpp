// n2p2 - A neural network potential package
// Copyright (C) 2018 Andreas Singraber (University of Vienna)
// Copyright (C) 2020 Martin P. Bircher
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "SymGrpCompAngn.h"
#include "Atom.h"
#include "SymFnc.h"
#include "SymFncCompAngn.h"
#include "Vec3D.h"
#include "utility.h"
#include <algorithm> // std::sort
#include <cmath>     // exp
#include <stdexcept> // std::runtime_error
using namespace std;
using namespace nnp;

SymGrpCompAngn::
SymGrpCompAngn(ElementMap const& elementMap) :
    SymGrpBaseCompAng(21, elementMap)
{
}

bool SymGrpCompAngn::operator==(SymGrp const& rhs) const
{
    if (ec   != rhs.getEc()  ) return false;
    if (type != rhs.getType()) return false;
    SymGrpCompAngn const& c = dynamic_cast<SymGrpCompAngn const&>(rhs);
    if (e1 != c.e1) return false;
    if (e2 != c.e2) return false;
    return true;
}

bool SymGrpCompAngn::operator<(SymGrp const& rhs) const
{
    if      (ec   < rhs.getEc()  ) return true;
    else if (ec   > rhs.getEc()  ) return false;
    if      (type < rhs.getType()) return true;
    else if (type > rhs.getType()) return false;
    SymGrpCompAngn const& c = dynamic_cast<SymGrpCompAngn const&>(rhs);
    if      (e1 < c.e1) return true;
    else if (e1 > c.e1) return false;
    if      (e2 < c.e2) return true;
    else if (e2 > c.e2) return false;
    return false;
}

bool SymGrpCompAngn::addMember(SymFnc const* const symmetryFunction)
{
    if (symmetryFunction->getType() != type) return false;

    SymFncCompAngn const* sf =
        dynamic_cast<SymFncCompAngn const*>(symmetryFunction);

    if (members.empty())
    {
        ec          = sf->getEc();
        e1          = sf->getE1();
        e2          = sf->getE2();
        convLength  = sf->getConvLength();
    }

    if (sf->getEc()          != ec         ) return false;
    if (sf->getE1()          != e1         ) return false;
    if (sf->getE2()          != e2         ) return false;
    if (sf->getConvLength()  != convLength )
    {
        throw runtime_error("ERROR: Unable to add symmetry function members "
                            "with different conversion factors.\n");
    }

    if (sf->getRl() <= 0.0) rmin = 0.0;
    else rmin = min( rmin, sf->getRl() );
    rmax = max( rmax, sf->getRc() );

    members.push_back(sf);

    return true;
}

void SymGrpCompAngn::sortMembers()
{
    sort(members.begin(),
         members.end(),
         comparePointerTargets<SymFncCompAngn const>);

    mrl.resize(members.size(), 0.0);
    mrc.resize(members.size(), 0.0);
    mal.resize(members.size(), 0.0);
    mar.resize(members.size(), 0.0);
    for (size_t i = 0; i < members.size(); i++)
    {
        mrl.at(i) = members[i]->getRl();
        mrc.at(i) = members[i]->getRc();
        mal.at(i) = members[i]->getAngleLeft() * M_PI / 180.0;
        mar.at(i) = members[i]->getAngleRight() * M_PI / 180.0;
        memberIndex.push_back(members[i]->getIndex());
        memberIndexPerElement.push_back(members[i]->getIndexPerElement());
        if (i == 0)
        {
            calculateComp.push_back(true);
        }
        else
        {
            if ( members[i - 1]->getRc() != members[i]->getRc() ||
                 members[i - 1]->getRl() != members[i]->getRl() )
            {
                calculateComp.push_back(true);
            }
            else
            {
                calculateComp.push_back(false);
            }
        }
    }

#ifndef N2P2_NO_SF_CACHE
    mci.resize(members.size());
    for (size_t k = 0; k < members.size(); ++k)
    {
        mci.at(k) = members.at(k)->getCacheIndices();
    }
#endif

    return;
}

// Depending on chosen symmetry functions this function may be very
// time-critical when predicting new structures (e.g. in MD simulations). Thus,
// lots of optimizations were used sacrificing some readablity. Vec3D
// operations have been rewritten in simple C array style and the use of
// temporary objects has been minimized. Some of the originally coded
// expressions are kept in comments marked with "SIMPLE EXPRESSIONS:".
void SymGrpCompAngn::calculate(Atom& atom, bool const derivatives) const
{
    double* result = new double[members.size()];
    double* radij  = new double[members.size()];
    double* dradij = new double[members.size()];
    for (size_t l = 0; l < members.size(); ++l)
    {
        result[l] = 0.0;
        radij[l]  = 0.0;
        dradij[l] = 0.0;
    }

    double r2min = rmin * rmin;
    double r2max = rmax * rmax;

    size_t numNeighbors = atom.numNeighbors;
    // Prevent problematic condition in loop test below (j < numNeighbors - 1).
    if (numNeighbors == 0) numNeighbors = 1;

    for (size_t j = 0; j < numNeighbors - 1; j++)
    {
        Atom::Neighbor& nj = atom.neighbors[j];
        size_t const nej = nj.element;
        double const rij = nj.d;

        if ((e1 == nej || e2 == nej) && rij < rmax && rij > rmin)
        {
            // Precalculate the radial part for ij
            // Supposedly saves quite a number of operations
            for (size_t l = 0; l < members.size(); ++l)
            {
                if (rij > mrl[l] && rij < mrc[l])
                {
                    SymFncCompAngn const& sf = *(members[l]);
#ifndef N2P2_NO_SF_CACHE
                    if (mci[l][nej].size() == 0)
                    {
                        sf.getCompactRadial(rij, radij[l], dradij[l]);
                    }
                    else
                    {
                        double& crad = nj.cache[mci[l][nej][0]];
                        double& cdrad = nj.cache[mci[l][nej][1]];
                        if (crad < 0) sf.getCompactRadial(rij, crad, cdrad);
                        radij[l] = crad;
                        dradij[l] = cdrad;
                    }
#else
                    sf.getCompactRadial(rij, radij[l], dradij[l]);
#endif
                }
                else radij[l] = 0.0;
            }

            // SIMPLE EXPRESSIONS:
            //Vec3D drij(atom.neighbors[j].dr);
            double const* const dr1 = nj.dr.r;

            for (size_t k = j + 1; k < numNeighbors; k++)
            {
                Atom::Neighbor& nk = atom.neighbors[k];
                size_t const nek = nk.element;
                if ((e1 == nej && e2 == nek) ||
                    (e2 == nej && e1 == nek))
                {
                    double const rik = nk.d;
                    if (rik < rmax && rik > rmin)
                    {
                        // SIMPLE EXPRESSIONS:
                        //Vec3D drik(atom.neighbors[k].dr);
                        //Vec3D drjk = drik - drij;
                        double const* const dr2 = nk.dr.r;
                        double const dr30 = dr2[0] - dr1[0];
                        double const dr31 = dr2[1] - dr1[1];
                        double const dr32 = dr2[2] - dr1[2];

                        double rjk = dr30 * dr30
                                   + dr31 * dr31
                                   + dr32 * dr32;
                        if ((rjk >= r2max) || (rjk <= r2min)) continue;
                        rjk = sqrt(rjk);

                        // Energy calculation.
                        double const rinvijik = 1.0 / (rij * rik);
                        double const costijk = (dr1[0] * dr2[0] +
                                                dr1[1] * dr2[1] +
                                                dr1[2] * dr2[2]) * rinvijik;

                        // By definition, our polynomial is zero at 0 and 180 deg.
                        // Therefore, skip the whole rest which might yield some NaN
                        if (costijk <= -1.0 || costijk >= 1.0) continue;

                        double const acostijk = acos(costijk);

                        double const rinvij   = rinvijik * rik;
                        double const rinvik   = rinvijik * rij;
                        double const rinvjk   = 1.0 / rjk;
                        double const phiijik0 = rinvij
                                              * (rinvik - rinvij * costijk);
                        double const phiikij0 = rinvik
                                              * (rinvij - rinvik * costijk);
                        double dacostijk = 0.0;
                        if (derivatives)
                        {
                            dacostijk = -1.0 / sqrt(1.0 - costijk * costijk);
                        }

                        bool skip = false;
                        double ang;
                        double dang;
                        double radik;
                        double dradik;
                        double radjk;
                        double dradjk;

                        for (size_t l = 0; l < members.size(); ++l)
                        {
                            // Break conditions.
                            if (radij[l] == 0.0 ||
                                rik <= mrl[l] || rik >= mrc[l] ||
                                rjk <= mrl[l] || rjk >= mrc[l] ||
                                acostijk <= mal[l] || acostijk >= mar[l])
                            {
                                // Record if skipped.
                                skip = true;
                                continue;
                            }

                            SymFncCompAngn const& sf = *(members[l]);
#ifndef N2P2_NO_SF_CACHE
                            if (mci[l][nek].size() == 0)
                            {
                                sf.getCompactRadial(rik, radik, dradik);
                            }
                            else
                            {
                                double& crad = nk.cache[mci[l][nek][0]];
                                double& cdrad = nk.cache[mci[l][nek][1]];
                                if (crad < 0) sf.getCompactRadial(rik, crad, cdrad);
                                radik = crad;
                                dradik = cdrad;
                            }
#else
                            sf.getCompactRadial(rik, radik, dradik);
#endif
                            // Calculate anyway if skipped previously.
                            if (calculateComp[l] || skip)
                            {
                                sf.getCompactRadial(rjk, radjk, dradjk);
                                skip = false;
                            }

                            sf.getCompactAngle(acostijk, ang, dang);

                            double rad = radij[l] * radik * radjk;
                            result[l] += rad * ang;
            
                            // Force calculation.
                            if (!derivatives) continue;

                            dang *= dacostijk;
                            double const phiijik = phiijik0 * dang;
                            double const phiikij = phiikij0 * dang;
                            double const psiijik = rinvijik * dang;

                            double const chiij =  rinvij * dradij[l]
                                               *  radik  * radjk;
                            double const chiik =  rinvik * radij[l]
                                               *  dradik * radjk;
                            double const chijk = -rinvjk * radij[l]
                                               *  radik  * dradjk;

                            double p1;
                            double p2;
                            double p3;

                            if (dang != 0.0 && rad != 0.0)
                            {
                                rad  *= scalingFactors[l];
                                ang  *= scalingFactors[l];
                                p1 = rad * phiijik +  ang * chiij;
                                p2 = rad * phiikij +  ang * chiik;
                                p3 = rad * psiijik +  ang * chijk;
                            }
                            else if (ang != 0.0)
                            {
                                ang *= scalingFactors[l];

                                p1 = ang * chiij;
                                p2 = ang * chiik;
                                p3 = ang * chijk;
                            }
                            else continue;

                            // SIMPLE EXPRESSIONS:
                            // Save force contributions in Atom storage.
                            //atom.dGdr[memberIndex[l]] += p1 * drij
                            //                           + p2 * drik;
                            //atom.neighbors[j].
                            //    dGdr[memberIndex[l]] -= p1 * drij
                            //                          + p3 * drjk;
                            //atom.neighbors[k].
                            //    dGdr[memberIndex[l]] -= p2 * drik
                            //                          - p3 * drjk;

                            double const p1drijx = p1 * dr1[0];
                            double const p1drijy = p1 * dr1[1];
                            double const p1drijz = p1 * dr1[2];

                            double const p2drikx = p2 * dr2[0];
                            double const p2driky = p2 * dr2[1];
                            double const p2drikz = p2 * dr2[2];

                            double const p3drjkx = p3 * dr30;
                            double const p3drjky = p3 * dr31;
                            double const p3drjkz = p3 * dr32;

#ifndef N2P2_FULL_SFD_MEMORY
                            size_t li = memberIndex[l];
#else
                            size_t const li = memberIndex[l];
#endif
                            double* dGdr = atom.dGdr[li].r;
                            dGdr[0] += p1drijx + p2drikx;
                            dGdr[1] += p1drijy + p2driky;
                            dGdr[2] += p1drijz + p2drikz;

#ifndef N2P2_FULL_SFD_MEMORY
                            li = memberIndexPerElement[l][nej];
#endif
                            dGdr = nj.dGdr[li].r;
                            dGdr[0] -= p1drijx + p3drjkx;
                            dGdr[1] -= p1drijy + p3drjky;
                            dGdr[2] -= p1drijz + p3drjkz;

#ifndef N2P2_FULL_SFD_MEMORY
                            li = memberIndexPerElement[l][nek];
#endif
                            dGdr = nk.dGdr[li].r;
                            dGdr[0] -= p2drikx - p3drjkx;
                            dGdr[1] -= p2driky - p3drjky;
                            dGdr[2] -= p2drikz - p3drjkz;
                        } // l
                    } // rik <= rc
                } // elem
            } // k
        } // rij <= rc
    } // j

    for (size_t l = 0; l < members.size(); ++l)
    {
        atom.G[memberIndex[l]] = members[l]->scale(result[l]);
    }

    delete[] result;
    delete[] radij;
    delete[] dradij;

    return;
}
