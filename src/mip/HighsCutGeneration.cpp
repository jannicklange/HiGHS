/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                       */
/*    This file is part of the HiGHS linear optimization suite           */
/*                                                                       */
/*    Written and engineered 2008-2021 at the University of Edinburgh    */
/*                                                                       */
/*    Available as open-source under the MIT License                     */
/*                                                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#include "mip/HighsCutGeneration.h"

#include "mip/HighsMipSolverData.h"
#include "mip/HighsTransformedLp.h"
#include "util/HighsIntegers.h"

HighsCutGeneration::HighsCutGeneration(const HighsLpRelaxation& lpRelaxation,
                                       HighsCutPool& cutpool)
    : lpRelaxation(lpRelaxation),
      cutpool(cutpool),
      feastol(lpRelaxation.getMipSolver().mipdata_->feastol),
      epsilon(lpRelaxation.getMipSolver().mipdata_->epsilon) {}

bool HighsCutGeneration::determineCover(bool lpSol) {
  if (rhs <= 10 * feastol) return false;

  cover.clear();
  cover.reserve(rowlen);

  for (HighsInt j = 0; j != rowlen; ++j) {
    if (!lpRelaxation.isColIntegral(inds[j])) continue;

    if (solval[j] <= feastol) continue;

    cover.push_back(j);
  }

  HighsInt maxCoverSize = cover.size();
  HighsInt coversize = 0;
  coverweight = 0.0;
  if (lpSol) {
    // take all variables that sit at their upper bound always into the cover
    coversize = std::partition(cover.begin(), cover.end(),
                               [&](HighsInt j) {
                                 return solval[j] >= upper[j] - feastol;
                               }) -
                cover.begin();

    for (HighsInt i = 0; i != coversize; ++i) {
      HighsInt j = cover[i];

      assert(solval[j] >= upper[j] - feastol);

      coverweight += vals[j] * upper[j];
    }
  }

  // sort the remaining variables by the contribution to the rows activity in
  // the current solution
  std::sort(
      cover.begin() + coversize, cover.begin() + maxCoverSize,
      [&](HighsInt i, HighsInt j) {
        double contributionA = solval[i] * vals[i];
        double contributionB = solval[j] * vals[j];

        // for equal contributions take the larger coefficients first
        // because this makes some of the lifting functions more likely to
        // generate a facet
        if (std::abs(contributionA - contributionB) <= feastol) {
          // if the value is equal too, choose a random tiebreaker based
          // on hashing the column index and the current number of pool
          // cuts
          if (std::abs(vals[i] - vals[j]) <= feastol)
            return HighsHashHelpers::hash(std::make_pair(
                       uint32_t(inds[i]), uint32_t(cutpool.getNumCuts()))) >
                   HighsHashHelpers::hash(std::make_pair(
                       uint32_t(inds[j]), uint32_t(cutpool.getNumCuts())));
          return vals[i] > vals[j];
        }

        return contributionA > contributionB;
      });

  const double minlambda =
      std::max(10 * feastol, feastol * std::abs(double(rhs)));

  for (; coversize != maxCoverSize; ++coversize) {
    double lambda = double(coverweight - rhs);
    if (lambda > minlambda) break;

    HighsInt j = cover[coversize];
    coverweight += vals[j] * upper[j];
  }
  if (coversize == 0) return false;

  coverweight.renormalize();
  lambda = coverweight - rhs;

  if (lambda <= minlambda) return false;

  cover.resize(coversize);
  assert(lambda > feastol);

  return true;
}

void HighsCutGeneration::separateLiftedKnapsackCover() {
  const double feastol = lpRelaxation.getMipSolver().mipdata_->feastol;

  const HighsInt coversize = cover.size();

  std::vector<double> S;
  S.resize(coversize);
  std::vector<int8_t> coverflag;
  coverflag.resize(rowlen);
  std::sort(cover.begin(), cover.end(),
            [&](HighsInt a, HighsInt b) { return vals[a] > vals[b]; });

  HighsCDouble abartmp = vals[cover[0]];
  HighsCDouble sigma = lambda;
  for (HighsInt i = 1; i != coversize; ++i) {
    HighsCDouble delta = abartmp - vals[cover[i]];
    HighsCDouble kdelta = double(i) * delta;
    if (double(kdelta) < double(sigma)) {
      abartmp = vals[cover[i]];
      sigma -= kdelta;
    } else {
      abartmp -= sigma * (1.0 / i);
      sigma = 0.0;
      break;
    }
  }

  if (double(sigma) > 0) abartmp = HighsCDouble(rhs) / double(coversize);

  double abar = double(abartmp);

  HighsCDouble sum = 0.0;
  HighsInt cplussize = 0;
  for (HighsInt i = 0; i != coversize; ++i) {
    sum += std::min(abar, vals[cover[i]]);
    S[i] = double(sum);

    if (vals[cover[i]] > abar + feastol) {
      ++cplussize;
      coverflag[cover[i]] = 1;
    } else
      coverflag[cover[i]] = -1;
  }
  assert(std::abs(double(sum - rhs) / double(rhs)) <= 1e-14);
  bool halfintegral = false;

  /* define the lifting function */
  auto g = [&](double z) {
    double hfrac = z / abar;
    double coef = 0.0;

    HighsInt h = std::floor(hfrac + 0.5);
    if (h != 0 && std::abs(hfrac - h) * std::max(1.0, abar) <= epsilon &&
        h <= cplussize - 1) {
      halfintegral = true;
      coef = 0.5;
    }

    h = std::max(h - 1, HighsInt{0});
    for (; h < coversize; ++h) {
      if (z <= S[h] + feastol) break;
    }

    return coef + h;
  };

  rhs = coversize - 1;

  for (HighsInt i = 0; i != rowlen; ++i) {
    if (vals[i] == 0.0) continue;
    if (coverflag[i] == -1) {
      vals[i] = 1;
    } else {
      vals[i] = g(vals[i]);
    }
  }

  if (halfintegral) {
    rhs *= 2;
    for (HighsInt i = 0; i != rowlen; ++i) vals[i] *= 2;
  }

  // resulting cut is always integral
  integralSupport = true;
  integralCoefficients = true;
}

bool HighsCutGeneration::separateLiftedMixedBinaryCover() {
  HighsInt coversize = cover.size();
  std::vector<double> S;
  S.resize(coversize);
  std::vector<uint8_t> coverflag;
  coverflag.resize(rowlen);

  if (coversize == 0) return false;

  for (HighsInt i = 0; i != coversize; ++i) coverflag[cover[i]] = 1;

  std::sort(cover.begin(), cover.end(),
            [&](HighsInt a, HighsInt b) { return vals[a] > vals[b]; });
  HighsCDouble sum = 0;

  HighsInt p = coversize;
  for (HighsInt i = 0; i != coversize; ++i) {
    if (vals[cover[i]] - lambda <= epsilon) {
      p = i;
      break;
    }
    sum += vals[cover[i]];
    S[i] = double(sum);
  }
  if (p == 0) return false;
  /* define the lifting function */
  auto phi = [&](double a) {
    for (HighsInt i = 0; i < p; ++i) {
      if (a <= S[i] - lambda) return double(i * lambda);

      if (a <= S[i]) return double((i + 1) * lambda + (HighsCDouble(a) - S[i]));
    }

    return double(p * lambda + (HighsCDouble(a) - S[p - 1]));
  };

  rhs = -lambda;

  integralCoefficients = false;
  integralSupport = true;
  for (HighsInt i = 0; i != rowlen; ++i) {
    if (!lpRelaxation.isColIntegral(inds[i])) {
      if (vals[i] < 0)
        integralSupport = false;
      else
        vals[i] = 0;
      continue;
    }

    if (coverflag[i]) {
      vals[i] = std::min(vals[i], double(lambda));
      rhs += vals[i];
    } else {
      vals[i] = phi(vals[i]);
    }
  }

  return true;
}

bool HighsCutGeneration::separateLiftedMixedIntegerCover() {
  HighsInt coversize = cover.size();

  HighsInt l = -1;

  std::vector<uint8_t> coverflag;
  coverflag.resize(rowlen);
  for (HighsInt i : cover) coverflag[i] = 1;

  auto comp = [&](HighsInt a, HighsInt b) { return vals[a] > vals[b]; };
  std::sort(cover.begin(), cover.end(), comp);

  std::vector<HighsCDouble> a;
  std::vector<HighsCDouble> u;
  std::vector<HighsCDouble> m;

  a.resize(coversize);
  u.resize(coversize + 1);
  m.resize(coversize + 1);

  HighsCDouble usum = 0.0;
  HighsCDouble msum = 0.0;
  // set up the partial sums of the upper bounds, and the contributions
  for (HighsInt c = 0; c != coversize; ++c) {
    HighsInt i = cover[c];

    u[c] = usum;
    m[c] = msum;
    a[c] = vals[i];
    double ub = upper[i];
    usum += ub;
    msum += ub * a[c];
  }

  u[coversize] = usum;
  m[coversize] = msum;

  // determine which variable in the cover we want to create the MIR inequality
  // from which we lift we try to select a variable to have the highest chance
  // of satisfying the facet conditions for the superadditive lifting function
  // gamma to be satisfied.
  HighsInt lpos = -1;
  HighsInt bestlCplusend = -1;
  double bestlVal = 0.0;
  bool bestlAtUpper = true;

  for (HighsInt i = 0; i != coversize; ++i) {
    HighsInt j = cover[i];
    double ub = upper[j];

    bool atUpper = solval[j] >= ub - feastol;
    if (atUpper && !bestlAtUpper) continue;

    double mju = ub * vals[j];
    HighsCDouble mu = mju - lambda;

    if (mu <= 10 * feastol) continue;
    if (std::abs(vals[j]) < 1000 * feastol) continue;

    double mudival = double(mu / vals[j]);
    if (std::abs(std::round(mudival) - mudival) <= feastol) continue;
    double eta = ceil(mudival);

    HighsCDouble ulminusetaplusone = HighsCDouble(ub) - eta + 1.0;
    HighsCDouble cplusthreshold = ulminusetaplusone * vals[j];

    HighsInt cplusend =
        std::upper_bound(cover.begin(), cover.end(), double(cplusthreshold),
                         [&](double cplusthreshold, HighsInt i) {
                           return cplusthreshold > vals[i];
                         }) -
        cover.begin();

    HighsCDouble mcplus = m[cplusend];
    if (i < cplusend) mcplus -= mju;

    double jlVal = double(mcplus + eta * vals[j]);

    if (jlVal > bestlVal || (!atUpper && bestlAtUpper)) {
      lpos = i;
      bestlCplusend = cplusend;
      bestlVal = jlVal;
      bestlAtUpper = atUpper;
    }
  }

  if (lpos == -1) return false;

  l = cover[lpos];
  HighsCDouble al = vals[l];
  double upperl = upper[l];
  HighsCDouble mlu = upperl * al;
  HighsCDouble mu = mlu - lambda;

  a.resize(bestlCplusend);
  cover.resize(bestlCplusend);
  u.resize(bestlCplusend + 1);
  m.resize(bestlCplusend + 1);

  if (lpos < bestlCplusend) {
    a.erase(a.begin() + lpos);
    cover.erase(cover.begin() + lpos);
    u.erase(u.begin() + lpos + 1);
    m.erase(m.begin() + lpos + 1);
    for (HighsInt i = lpos + 1; i < bestlCplusend; ++i) {
      u[i] -= upperl;
      m[i] -= mlu;
    }
  }

  HighsInt cplussize = a.size();

  assert(mu > 10 * feastol);

  double mudival = double(mu / al);
  double eta = ceil(mudival);
  HighsCDouble r = mu - floor(mudival) * HighsCDouble(al);
  // we multiply with r and it is important that it does not flip the sign
  // so we safe guard against tiny numerical errors here
  if (r < 0) r = 0;

  HighsCDouble ulminusetaplusone = HighsCDouble(upperl) - eta + 1.0;
  HighsCDouble cplusthreshold = ulminusetaplusone * al;

  HighsInt kmin = floor(eta - upperl - 0.5);

  auto phi_l = [&](double a) {
    assert(a < 0);

    int64_t k = std::min(int64_t(a / double(al)), int64_t(-1));

    for (; k >= kmin; --k) {
      if (a >= k * al + r) {
        assert(a < (k + 1) * al);
        return double(a - (k + 1) * r);
      }

      if (a >= k * al) {
        assert(a < k * al + r);
        return double(k * (al - r));
      }
    }

    assert(a <= -lambda + epsilon);
    return double(kmin * (al - r));
  };

  int64_t kmax = floor(upperl - eta + 0.5);

  auto gamma_l = [&](double z) {
    assert(z > 0);
    for (HighsInt i = 0; i < cplussize; ++i) {
      HighsInt upperi = upper[cover[i]];

      for (HighsInt h = 0; h <= upperi; ++h) {
        HighsCDouble mih = m[i] + h * a[i];
        HighsCDouble uih = u[i] + h;
        HighsCDouble mihplusdeltai = mih + a[i] - cplusthreshold;
        if (z <= mihplusdeltai) {
          assert(mih <= z);
          return double(uih * ulminusetaplusone * (al - r));
        }

        int64_t k = ((int64_t)(double)((z - mihplusdeltai) / al)) - 1;
        for (; k <= kmax; ++k) {
          if (z <= mihplusdeltai + k * al + r) {
            assert(mihplusdeltai + k * al <= z);
            return double((uih * ulminusetaplusone + k) * (al - r));
          }

          if (z <= mihplusdeltai + (k + 1) * al) {
            assert(mihplusdeltai + k * al + r <= z);
            return double((uih * ulminusetaplusone) * (al - r) + z - mih -
                          a[i] + cplusthreshold - (k + 1) * r);
          }
        }
      }
    }

    int64_t p = ((int64_t)(double)((z - m[cplussize]) / al)) - 1;
    for (;; ++p) {
      if (z <= m[cplussize] + p * al + r) {
        assert(m[cplussize] + p * al <= z);
        return double((u[cplussize] * ulminusetaplusone + p) * (al - r));
      }

      if (z <= m[cplussize] + (p + 1) * al) {
        assert(m[cplussize] + p * al + r <= z);
        return double((u[cplussize] * ulminusetaplusone) * (al - r) + z -
                      m[cplussize] - (p + 1) * r);
      }
    }
  };

  rhs = (HighsCDouble(upperl) - eta) * r - lambda;
  integralSupport = true;
  integralCoefficients = false;
  for (HighsInt i = 0; i != rowlen; ++i) {
    if (vals[i] == 0.0) continue;
    HighsInt col = inds[i];

    if (!lpRelaxation.isColIntegral(col)) {
      if (vals[i] < 0.0)
        integralSupport = false;
      else
        vals[i] = 0.0;
      continue;
    }

    if (coverflag[i]) {
      vals[i] = -phi_l(-vals[i]);
      rhs += vals[i] * upper[i];
    } else {
      vals[i] = gamma_l(vals[i]);
    }
  }

  return true;
}

bool HighsCutGeneration::cmirCutGenerationHeuristic() {
  std::vector<double> deltas;

  HighsCDouble continuouscontribution = 0.0;
  HighsCDouble continuoussqrnorm = 0.0;
  std::vector<HighsInt> integerinds;
  integerinds.reserve(rowlen);
  double maxabsdelta = 0.0;

  complementation.resize(rowlen);

  for (HighsInt i = 0; i != rowlen; ++i) {
    if (lpRelaxation.isColIntegral(inds[i])) {
      integerinds.push_back(i);

      if (upper[i] < 2 * solval[i]) {
        complementation[i] = 1 - complementation[i];
        rhs -= upper[i] * vals[i];
        vals[i] = -vals[i];
        solval[i] = upper[i] - solval[i];
      }

      if (solval[i] > feastol) {
        double delta = std::abs(vals[i]);
        if (delta <= 1e-4 || delta >= 1e4) continue;
        maxabsdelta = std::max(maxabsdelta, delta);
        deltas.push_back(delta);
      }
    } else {
      continuouscontribution += vals[i] * solval[i];
      continuoussqrnorm += vals[i] * vals[i];
    }
  }

  if (maxabsdelta + 1.0 > 1e-4 && maxabsdelta + 1.0 < 1e4)
    deltas.push_back(maxabsdelta + 1.0);
  deltas.push_back(1.0);

  if (deltas.empty()) return false;

  std::sort(deltas.begin(), deltas.end());
  double curdelta = deltas[0];
  for (size_t i = 1; i < deltas.size(); ++i) {
    if (deltas[i] - curdelta <= feastol)
      deltas[i] = 0.0;
    else
      curdelta = deltas[i];
  }

  deltas.erase(std::remove(deltas.begin(), deltas.end(), 0.0), deltas.end());
  double bestdelta = -1;
  double bestefficacy = 0.0;

  for (double delta : deltas) {
    HighsCDouble scale = 1.0 / HighsCDouble(delta);
    HighsCDouble scalrhs = rhs * scale;
    double downrhs = std::floor(double(scalrhs));

    HighsCDouble f0 = scalrhs - downrhs;
    if (f0 < 0.01 || f0 > 0.99) continue;
    HighsCDouble oneoveroneminusf0 = 1.0 / (1.0 - f0);
    if (double(oneoveroneminusf0) * double(scale) > 1e4) continue;

    HighsCDouble sqrnorm = scale * scale * continuoussqrnorm;
    HighsCDouble viol = continuouscontribution * oneoveroneminusf0 - scalrhs;

    for (HighsInt j : integerinds) {
      HighsCDouble scalaj = vals[j] * scale;
      double downaj = std::floor(double(scalaj));
      HighsCDouble fj = scalaj - downaj;
      double aj;
      if (fj > f0)
        aj = double(downaj + fj - f0);
      else
        aj = downaj;

      viol += aj * solval[j];
      sqrnorm += aj * aj;
    }

    double efficacy = double(viol / sqrt(sqrnorm));
    if (efficacy > bestefficacy) {
      bestdelta = delta;
      bestefficacy = efficacy;
    }
  }

  if (bestdelta == -1) return false;

  /* try if multiplying best delta by 2 4 or 8 gives a better efficacy */
  for (HighsInt k = 1; k <= 3; ++k) {
    double delta = bestdelta * (1 << k);
    if (delta <= 1e-4 || delta >= 1e4) continue;
    HighsCDouble scale = 1.0 / HighsCDouble(delta);
    HighsCDouble scalrhs = rhs * scale;
    double downrhs = std::floor(double(scalrhs));
    HighsCDouble f0 = scalrhs - downrhs;
    if (f0 < 0.01 || f0 > 0.99) continue;

    HighsCDouble oneoveroneminusf0 = 1.0 / (1.0 - f0);
    if (double(oneoveroneminusf0) * double(scale) > 1e4) continue;

    HighsCDouble sqrnorm = scale * scale * continuoussqrnorm;
    HighsCDouble viol = continuouscontribution * oneoveroneminusf0 - scalrhs;

    for (HighsInt j : integerinds) {
      HighsCDouble scalaj = vals[j] * scale;
      double downaj = std::floor(double(scalaj));
      HighsCDouble fj = scalaj - downaj;
      double aj;
      if (fj > f0)
        aj = double(downaj + fj - f0);
      else
        aj = downaj;

      viol += aj * solval[j];
      sqrnorm += aj * aj;
    }

    double efficacy = double(viol / sqrt(sqrnorm));
    if (efficacy > bestefficacy) {
      bestdelta = delta;
      bestefficacy = efficacy;
    }
  }

  if (bestdelta == -1) return false;

  // try to flip complementation of integers to increase efficacy

  for (HighsInt k : integerinds) {
    if (upper[k] == HIGHS_CONST_INF) continue;

    complementation[k] = 1 - complementation[k];
    solval[k] = upper[k] - solval[k];
    rhs -= upper[k] * vals[k];
    vals[k] = -vals[k];

    double delta = bestdelta;
    HighsCDouble scale = 1.0 / HighsCDouble(delta);
    HighsCDouble scalrhs = rhs * scale;
    double downrhs = std::floor(double(scalrhs));

    HighsCDouble f0 = scalrhs - downrhs;
    if (f0 < 0.01 || f0 > 0.99) {
      complementation[k] = 1 - complementation[k];
      solval[k] = upper[k] - solval[k];
      rhs -= upper[k] * vals[k];
      vals[k] = -vals[k];

      continue;
    }

    HighsCDouble oneoveroneminusf0 = 1.0 / (1.0 - f0);
    if (double(oneoveroneminusf0) * double(scale) > 1e4) {
      complementation[k] = 1 - complementation[k];
      solval[k] = upper[k] - solval[k];
      rhs -= upper[k] * vals[k];
      vals[k] = -vals[k];

      continue;
    }

    HighsCDouble sqrnorm = scale * scale * continuoussqrnorm;
    HighsCDouble viol = continuouscontribution * oneoveroneminusf0 - scalrhs;

    for (HighsInt j : integerinds) {
      HighsCDouble scalaj = vals[j] * scale;
      double downaj = std::floor(double(scalaj));
      HighsCDouble fj = scalaj - downaj;
      double aj;
      if (fj > f0)
        aj = double(downaj + fj - f0);
      else
        aj = downaj;

      viol += aj * solval[j];
      sqrnorm += aj * aj;
    }

    double efficacy = double(viol / sqrt(sqrnorm));
    if (efficacy > bestefficacy) {
      bestefficacy = efficacy;
    } else {
      complementation[k] = 1 - complementation[k];
      solval[k] = upper[k] - solval[k];
      rhs -= upper[k] * vals[k];
      vals[k] = -vals[k];
    }
  }

  HighsCDouble scale = 1.0 / HighsCDouble(bestdelta);
  HighsCDouble scalrhs = rhs * scale;
  double downrhs = std::floor(double(scalrhs));

  HighsCDouble f0 = scalrhs - downrhs;
  HighsCDouble oneoveroneminusf0 = 1.0 / (1.0 - f0);

  rhs = downrhs * bestdelta;
  integralSupport = true;
  integralCoefficients = false;
  for (HighsInt j = 0; j != rowlen; ++j) {
    if (vals[j] == 0.0) continue;
    if (!lpRelaxation.isColIntegral(inds[j])) {
      if (vals[j] > 0.0)
        vals[j] = 0.0;
      else {
        vals[j] = double(vals[j] * oneoveroneminusf0);
        integralSupport = false;
      }
    } else {
      HighsCDouble scalaj = scale * vals[j];
      double downaj = std::floor(double(scalaj));
      HighsCDouble fj = scalaj - downaj;
      HighsCDouble aj;
      if (fj > f0)
        aj = downaj + fj - f0;
      else
        aj = downaj;
      vals[j] = double(aj * bestdelta);
    }
  }

  return true;
}

bool HighsCutGeneration::postprocessCut() {
  double maxAbsValue;
  if (integralSupport) {
    if (integralCoefficients) return true;

    // if the support is integral, allow a maximal dynamism of 1e4
    maxAbsValue = 0.0;
    for (HighsInt i = 0; i != rowlen; ++i)
      maxAbsValue = std::max(std::abs(vals[i]), maxAbsValue);

    double minCoefficientValue = std::max(maxAbsValue * 100 * feastol, epsilon);

    for (HighsInt i = 0; i != rowlen; ++i) {
      if (vals[i] == 0) continue;
      if (std::abs(vals[i]) <= minCoefficientValue) {
        if (vals[i] < 0) {
          double ub = upper[i];
          if (ub == HIGHS_CONST_INF)
            return false;
          else
            rhs -= ub * vals[i];
        }

        vals[i] = 0.0;
      }
    }

    std::vector<double> nonzerovals;
    nonzerovals.reserve(rowlen);

    for (HighsInt i = 0; i != rowlen; ++i)
      if (vals[i] != 0) nonzerovals.push_back(vals[i]);

    double intscale =
        HighsIntegers::integralScale(nonzerovals, feastol, epsilon);

    bool scaleSmallestValToOne = true;

    if (intscale != 0.0 &&
        intscale * std::max(1.0, maxAbsValue) <= (double)(uint64_t{1} << 53)) {
      // A scale to make all value integral was found. The scale is only
      // rejected if it is in a range where not all integral values are
      // representable in double precision anymore. Otherwise we want to always
      // use the scale to adjust the coefficients and right hand side for
      // numerical safety reasons. If the resulting integral values are too
      // large, however, we scale the cut down by shifting the exponent.
      rhs.renormalize();
      rhs *= intscale;
      maxAbsValue = std::round(maxAbsValue * intscale);
      for (HighsInt i = 0; i != rowlen; ++i) {
        if (vals[i] == 0.0) continue;

        HighsCDouble scaleval = intscale * HighsCDouble(vals[i]);
        HighsCDouble intval = round(scaleval);
        double delta = double(scaleval - intval);

        vals[i] = (double)intval;

        // if the coefficient would be strengthened by rounding, we add the
        // upperbound constraint to make it exactly integral instead and
        // therefore weaken the right hand side
        if (delta < 0.0) {
          if (upper[i] == HIGHS_CONST_INF) return false;

          rhs -= delta * upper[i];
        }
      }

      // finally we can round down the right hand side. Therefore in most cases
      // small errors for which the upper bound constraints where used and the
      // right hand side was weakened, do not weaken the final cut.
      rhs = floor(rhs + epsilon);

      if (intscale * maxAbsValue * feastol <= 1.0) {
        scaleSmallestValToOne = false;
        integralCoefficients = true;
      }
    }

    if (scaleSmallestValToOne) {
      double minAbsValue = HIGHS_CONST_INF;
      for (HighsInt i = 0; i != rowlen; ++i) {
        if (vals[i] == 0.0) continue;
        minAbsValue = std::min(std::abs(vals[i]), minAbsValue);
      }

      int expshift;
      std::frexp(minAbsValue - epsilon, &expshift);
      expshift = -expshift;

      maxAbsValue = std::ldexp(maxAbsValue, expshift);

      rhs = std::ldexp((double)rhs, expshift);

      for (HighsInt i = 0; i != rowlen; ++i) {
        if (vals[i] == 0) continue;

        vals[i] = std::ldexp(vals[i], expshift);
      }
    }
  } else {
    maxAbsValue = 0.0;
    for (HighsInt i = 0; i != rowlen; ++i)
      maxAbsValue = std::max(std::abs(vals[i]), maxAbsValue);

    int expshift;
    std::frexp(maxAbsValue, &expshift);
    expshift = -expshift;

    double minCoefficientValue =
        std::ldexp(maxAbsValue * 100 * feastol, expshift);
    rhs = std::ldexp((double)rhs, expshift);

    // now remove small coefficients and determine the smallest absolute
    // coefficient of an integral variable
    double minIntCoef = HIGHS_CONST_INF;
    for (HighsInt i = 0; i != rowlen; ++i) {
      if (vals[i] == 0.0) continue;

      vals[i] = std::ldexp(vals[i], expshift);

      if (std::abs(vals[i]) <= minCoefficientValue) {
        if (vals[i] < 0.0) {
          if (upper[i] == HIGHS_CONST_INF) return false;
          rhs -= vals[i] * upper[i];
        } else
          vals[i] = 0.0;
      }
    }
  }

  return true;
}

bool HighsCutGeneration::preprocessBaseInequality(bool& hasUnboundedInts,
                                                  bool& hasGeneralInts,
                                                  bool& hasContinuous) {
  // preprocess the inequality before cut generation
  // 1. Determine the maximal activity to check for trivial redundancy and
  // tighten coefficients
  // 2. Check for presence of continuous variables and unbounded integers as not
  // all methods for cut generation are applicable in that case
  // 3. Remove coefficients that are below the feasibility tolerance to avoid
  // numerical troubles, use bound constraints to cancel them and
  // reject base inequalities where that is not possible due to unbounded
  // variables
  hasUnboundedInts = false;
  hasContinuous = false;
  hasGeneralInts = false;
  HighsInt numZeros = 0;

  double maxact = -feastol;
  double maxAbsVal = 0;
  for (HighsInt i = 0; i < rowlen; ++i)
    maxAbsVal = std::max(std::abs(vals[i]), maxAbsVal);

  int expshift = 0;
  std::frexp(maxAbsVal, &expshift);
  expshift = -expshift;
  rhs *= std::ldexp(1.0, expshift);

  for (HighsInt i = 0; i != rowlen; ++i) {
    vals[i] = std::ldexp(vals[i], expshift);
    if (std::abs(vals[i]) <= feastol) {
      if (vals[i] < 0) {
        if (upper[i] == HIGHS_CONST_INF) return false;
        rhs -= vals[i] * upper[i];
      }

      ++numZeros;
      vals[i] = 0.0;
      continue;
    }

    if (!lpRelaxation.isColIntegral(inds[i])) {
      hasContinuous = true;

      if (vals[i] > 0) {
        if (upper[i] == HIGHS_CONST_INF)
          maxact = HIGHS_CONST_INF;
        else
          maxact += vals[i] * upper[i];
      }
    } else {
      if (upper[i] == HIGHS_CONST_INF) {
        hasUnboundedInts = true;
        hasGeneralInts = true;
        if (vals[i] > 0.0) maxact = HIGHS_CONST_INF;
        if (maxact == HIGHS_CONST_INF) break;
      } else if (upper[i] != 1.0) {
        hasGeneralInts = true;
      }

      if (vals[i] > 0) maxact += vals[i] * upper[i];
    }
  }

  HighsInt maxLen = 100 + 0.15 * (lpRelaxation.numCols());

  if (rowlen - numZeros > maxLen) {
    HighsInt numCancel = rowlen - numZeros - maxLen;
    std::vector<HighsInt> cancelNzs;

    for (HighsInt i = 0; i != rowlen; ++i) {
      double cancelSlack = vals[i] > 0 ? solval[i] : upper[i] - solval[i];
      if (cancelSlack <= feastol) cancelNzs.push_back(i);
    }

    if ((HighsInt)cancelNzs.size() < numCancel) return false;
    if ((HighsInt)cancelNzs.size() > numCancel)
      std::partial_sort(cancelNzs.begin(), cancelNzs.begin() + numCancel,
                        cancelNzs.end(), [&](HighsInt a, HighsInt b) {
                          return std::abs(vals[a]) < std::abs(vals[b]);
                        });

    for (HighsInt i = 0; i < numCancel; ++i) {
      HighsInt j = cancelNzs[i];

      if (vals[j] < 0) {
        rhs -= vals[j] * upper[j];
      } else
        maxact -= vals[j] * upper[j];

      vals[j] = 0.0;
    }

    numZeros += numCancel;
  }

  if (numZeros != 0) {
    // remove zeros in place
    if (complementation.empty()) {
      for (HighsInt i = rowlen - 1; i >= 0; --i) {
        if (vals[i] == 0.0) {
          --rowlen;
          inds[i] = inds[rowlen];
          vals[i] = vals[rowlen];
          upper[i] = upper[rowlen];
          solval[i] = solval[rowlen];
          if (--numZeros == 0) break;
        }
      }
    } else {
      for (HighsInt i = rowlen - 1; i >= 0; --i) {
        if (vals[i] == 0.0) {
          --rowlen;
          inds[i] = inds[rowlen];
          vals[i] = vals[rowlen];
          upper[i] = upper[rowlen];
          solval[i] = solval[rowlen];
          complementation[i] = complementation[rowlen];
          if (--numZeros == 0) break;
        }
      }
    }
  }

  return maxact > rhs;
}

static void checkNumerics(const double* vals, HighsInt len, double rhs) {
  double maxAbsCoef = 0.0;
  double minAbsCoef = HIGHS_CONST_INF;
  HighsCDouble sqrnorm = 0;
  for (HighsInt i = 0; i < len; ++i) {
    sqrnorm += vals[i] * vals[i];
    maxAbsCoef = std::max(std::abs(vals[i]), maxAbsCoef);
    minAbsCoef = std::min(std::abs(vals[i]), minAbsCoef);
  }

  double norm = double(sqrt(sqrnorm));

  // printf("length: %" HIGHSINT_FORMAT
  //       ", minCoef: %g, maxCoef, %g, norm %g, rhs: %g, dynamism=%g\n",
  //       len, minAbsCoef, maxAbsCoef, norm, rhs, maxAbsCoef / minAbsCoef);
}

bool HighsCutGeneration::generateCut(HighsTransformedLp& transLp,
                                     std::vector<HighsInt>& inds_,
                                     std::vector<double>& vals_, double& rhs_) {
  bool intsPositive = true;
  if (!transLp.transform(vals_, upper, solval, inds_, rhs_, intsPositive))
    return false;

#if 1
  if (vals_.size() > 1) {
    std::vector<HighsInt> indsCheck_ = inds_;
    std::vector<double> valsCheck_ = vals_;
    rowlen = indsCheck_.size();
    this->inds = indsCheck_.data();
    this->vals = valsCheck_.data();
    this->rhs = rhs_;
    complementation.clear();
    bool hasUnboundedInts = false;
    bool hasGeneralInts = false;
    bool hasContinuous = false;
    bool oldIntsPositive = intsPositive;
    // printf("before preprocessing of base inequality:\n");
    checkNumerics(vals, rowlen, double(rhs));
    if (!preprocessBaseInequality(hasUnboundedInts, hasGeneralInts,
                                  hasContinuous))
      return false;
    // printf("after preprocessing of base inequality:\n");
    checkNumerics(vals, rowlen, double(rhs));

    double tmprhs_ = (double)rhs;
    valsCheck_.resize(rowlen);
    indsCheck_.resize(rowlen);
    if (!transLp.untransform(valsCheck_, indsCheck_, tmprhs_)) return false;

    // printf("after untransform of base inequality:\n");
    checkNumerics(vals, rowlen, double(rhs));

    // finally check whether the cut is violated
    rowlen = indsCheck_.size();
    inds = indsCheck_.data();
    vals = valsCheck_.data();
    lpRelaxation.getMipSolver().mipdata_->debugSolution.checkCut(
        inds, vals, rowlen, tmprhs_);

    intsPositive = oldIntsPositive;
  }
#endif
  rowlen = inds_.size();
  this->inds = inds_.data();
  this->vals = vals_.data();
  this->rhs = rhs_;
  complementation.clear();
  bool hasUnboundedInts = false;
  bool hasGeneralInts = false;
  bool hasContinuous = false;
  if (!preprocessBaseInequality(hasUnboundedInts, hasGeneralInts,
                                hasContinuous))
    return false;

  // it can happen that there is an unbounded integer variable during the
  // transform call so that the integers are not tranformed to positive values.
  // Now the call to preprocessBaseInequality may have removed the unbounded
  // integer, e.g. due to a small coefficient value, so that we can still use
  // the lifted inequalities instead of cmir. We need to make sure, however,
  // that the cut values are transformed to positive coefficients first, which
  // we do below.
  if (!hasUnboundedInts && !intsPositive) {
    complementation.resize(rowlen);

    for (HighsInt i = 0; i != rowlen; ++i) {
      if (vals[i] > 0 || !lpRelaxation.isColIntegral(inds[i])) continue;

      complementation[i] = 1 - complementation[i];
      rhs -= upper[i] * vals[i];
      vals[i] = -vals[i];
    }
  }

  if (hasUnboundedInts) {
    if (!cmirCutGenerationHeuristic()) return false;
  } else {
    // 1. Determine a cover, cover does not need to be minimal as neither of
    // the
    //    lifting functions have minimality of the cover as necessary facet
    //    condition
    if (!determineCover()) return false;

    // 2. use superadditive lifting function depending on structure of base
    //    inequality:
    //    We have 3 lifting functions available for pure binary knapsack sets,
    //    for mixed-binary knapsack sets and for mixed integer knapsack sets.
    if (!hasContinuous && !hasGeneralInts)
      separateLiftedKnapsackCover();
    else if (hasGeneralInts) {
      if (!separateLiftedMixedIntegerCover()) return false;
    } else {
      assert(hasContinuous);
      assert(!hasGeneralInts);
      if (!separateLiftedMixedBinaryCover()) return false;
    }
  }

  // apply cut postprocessing including scaling and removal of small
  // coeffiicents
  if (!postprocessCut()) return false;

  if (!complementation.empty()) {
    // remove the complementation if exists
    for (HighsInt i = 0; i != rowlen; ++i) {
      if (complementation[i]) {
        rhs -= upper[i] * vals[i];
        vals[i] = -vals[i];
      }
    }
  }

  // transform the cut back into the original space, i.e. remove the bound
  // substitution and replace implicit slack variables
  rhs_ = (double)rhs;
  bool cutintegral = integralSupport && integralCoefficients;
  vals_.resize(rowlen);
  inds_.resize(rowlen);
  if (!transLp.untransform(vals_, inds_, rhs_, cutintegral)) return false;

  // finally check whether the cut is violated
  rowlen = inds_.size();
  inds = inds_.data();
  vals = vals_.data();
  lpRelaxation.getMipSolver().mipdata_->debugSolution.checkCut(inds, vals,
                                                               rowlen, rhs_);

  // finally determine the violation of the cut in the original space
  HighsCDouble violation = -rhs_;
  const auto& sol = lpRelaxation.getSolution().col_value;
  for (HighsInt i = 0; i != rowlen; ++i) violation += sol[inds[i]] * vals_[i];

  if (violation <= 10 * feastol) return false;

  lpRelaxation.getMipSolver().mipdata_->domain.tightenCoefficients(
      inds, vals, rowlen, rhs_);

  // if the cut is violated by a small factor above the feasibility
  // tolerance, add it to the cutpool
  HighsInt cutindex =
      cutpool.addCut(lpRelaxation.getMipSolver(), inds_.data(), vals_.data(),
                     inds_.size(), rhs_, cutintegral);

  // only return true if cut was accepted by the cutpool, i.e. not a duplicate
  // of a cut already in the pool
  return cutindex != -1;
}

bool HighsCutGeneration::generateConflict(HighsDomain& localdomain,
                                          std::vector<HighsInt>& proofinds,
                                          std::vector<double>& proofvals,
                                          double& proofrhs) {
  this->inds = proofinds.data();
  this->vals = proofvals.data();
  this->rhs = proofrhs;
  rowlen = proofinds.size();

  lpRelaxation.getMipSolver().mipdata_->debugSolution.checkCut(
      inds, vals, rowlen, proofrhs);

  complementation.assign(rowlen, 0);

  upper.resize(rowlen);
  solval.resize(rowlen);

  HighsDomain& globaldomain = lpRelaxation.getMipSolver().mipdata_->domain;
  for (HighsInt i = 0; i != rowlen; ++i) {
    HighsInt col = inds[i];

    upper[i] = globaldomain.colUpper_[col] - globaldomain.colLower_[col];

    if (vals[i] < 0 && globaldomain.colUpper_[col] != HIGHS_CONST_INF) {
      rhs -= globaldomain.colUpper_[col] * vals[i];
      vals[i] = -vals[i];
      complementation[i] = 1;

      solval[i] = globaldomain.colUpper_[col] - localdomain.colUpper_[col];
    } else {
      rhs -= globaldomain.colLower_[col] * vals[i];
      complementation[i] = 0;
      solval[i] = localdomain.colLower_[col] - globaldomain.colLower_[col];
    }
  }

  bool hasUnboundedInts = false;
  bool hasGeneralInts = false;
  bool hasContinuous = false;

  if (!preprocessBaseInequality(hasUnboundedInts, hasGeneralInts,
                                hasContinuous))
    return false;

  if (hasUnboundedInts) {
    if (!cmirCutGenerationHeuristic()) return false;
  } else {
    // 1. Determine a cover, cover does not need to be minimal as neither of the
    //    lifting functions have minimality of the cover as necessary facet
    //    condition
    if (!determineCover(false)) return false;

    // 2. use superadditive lifting function depending on structure of base
    //    inequality:
    //    We have 3 lifting functions available for pure binary knapsack sets,
    //    for mixed-binary knapsack sets and for mixed integer knapsack sets.
    if (!hasContinuous && !hasGeneralInts)
      separateLiftedKnapsackCover();
    else if (hasGeneralInts) {
      if (!separateLiftedMixedIntegerCover()) return false;
    } else {
      assert(hasContinuous);
      assert(!hasGeneralInts);
      if (!separateLiftedMixedBinaryCover()) return false;
    }
  }

  // apply cut postprocessing including scaling and removal of small
  // coefficients
  if (!postprocessCut()) return false;

  // remove the complementation
  for (HighsInt i = 0; i != rowlen; ++i) {
    if (complementation[i]) {
      rhs -= globaldomain.colUpper_[inds[i]] * vals[i];
      vals[i] = -vals[i];
    } else
      rhs += globaldomain.colLower_[inds[i]] * vals[i];
  }

  // remove zeros in place
  for (HighsInt i = rowlen - 1; i >= 0; --i) {
    if (vals[i] == 0.0) {
      --rowlen;
      proofinds[i] = proofinds[rowlen];
      proofvals[i] = proofvals[rowlen];
    }
  }

  proofvals.resize(rowlen);
  proofinds.resize(rowlen);
  proofrhs = (double)rhs;

  bool cutintegral = integralSupport && integralCoefficients;

  lpRelaxation.getMipSolver().mipdata_->domain.tightenCoefficients(
      proofinds.data(), proofvals.data(), rowlen, proofrhs);

  HighsInt cutindex =
      cutpool.addCut(lpRelaxation.getMipSolver(), proofinds.data(),
                     proofvals.data(), rowlen, proofrhs, cutintegral);

  // only return true if cut was accepted by the cutpool, i.e. not a duplicate
  // of a cut already in the pool
  return cutindex != -1;
}
