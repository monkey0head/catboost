#include "fold.h"
#include "train_data.h"

#include <catboost/libs/helpers/restorable_rng.h>
#include <catboost/libs/helpers/permutation.h>

TVector<int> InvertPermutation(const TVector<int>& permutation) {
    TVector<int> result(permutation.size());
    for (size_t i = 0; i < permutation.size(); ++i) {
        result[permutation[i]] = i;
    }
    return result;
}

int UpdateSizeForQueries(int size, const TVector<int>& queriesFinishIndex) {
    if (!queriesFinishIndex.empty()) {
        double meanQuerySize = queriesFinishIndex.back() / (queriesFinishIndex.ysize() + 1);
        int queryIndex = Min(static_cast<int>(ceil(size / meanQuerySize)) - 1, queriesFinishIndex.ysize() - 1);
        while (queriesFinishIndex[queryIndex] < size && queryIndex < queriesFinishIndex.ysize() - 1) {
            ++queryIndex;
        }
        size = queriesFinishIndex[queryIndex];
    }
    return size;
}


int SelectMinBatchSize(const int sampleCount, const TVector<int>& queriesFinishIndex) {
    int size = sampleCount > 500 ? Min<int>(100, sampleCount / 50) : 1;
    return UpdateSizeForQueries(size, queriesFinishIndex);
}


double SelectTailSize(const int oldSize, const double multiplier, const TVector<int>& queriesFinishIndex) {
    int size = ceil(oldSize * multiplier);
    return UpdateSizeForQueries(size, queriesFinishIndex);
}

void InitFromBaseline(
    const int beginIdx,
    const int endIdx,
    const TVector<TVector<double>>& baseline,
    const TVector<int>& learnPermutation,
    bool storeExpApproxes,
    TVector<TVector<double>>* approx
) {
    const int learnSampleCount = learnPermutation.ysize();
    const int approxDimension = approx->ysize();
    for (int dim = 0; dim < approxDimension; ++dim) {
        TVector<double> tempBaseline(baseline[dim]);
        ExpApproxIf(storeExpApproxes, &tempBaseline);
        for (int docId = beginIdx; docId < endIdx; ++docId) {
            int initialIdx = docId;
            if (docId < learnSampleCount) {
                initialIdx = learnPermutation[docId];
            }
            (*approx)[dim][docId] = tempBaseline[initialIdx];
        }
    }
}


TVector<int> CalcQueriesFinishIndex(const TVector<ui32>& queriesId) {
    TVector<int> queriesFinishIndex;
    if (queriesId.empty()) {
        return queriesFinishIndex;
    }
    ui32 lastQueryId = queriesId[0];
    for (int docId = 0; docId < queriesId.ysize(); ++docId) {
        if (queriesId[docId] != lastQueryId) {
            queriesFinishIndex.push_back(docId);
            lastQueryId = queriesId[docId];
        }
    }
    queriesFinishIndex.push_back(queriesId.ysize());
    return queriesFinishIndex;
}

static void ShuffleData(const TTrainData& data, int permuteBlockSize, TRestorableFastRng64& rand, TFold* fold) {
    if (permuteBlockSize == 1 || !data.QueryId.empty()) {
        Shuffle(data.QueryId, rand, &fold->LearnPermutation);
        fold->PermutationBlockSize = 1;
    } else {
        const int blocksCount = (data.LearnSampleCount + permuteBlockSize - 1) / permuteBlockSize;
        TVector<int> blockedPermute(blocksCount);
        std::iota(blockedPermute.begin(), blockedPermute.end(), 0);
        Shuffle(blockedPermute.begin(), blockedPermute.end(), rand);

        int currentIdx = 0;
        for (int i = 0; i < blocksCount; ++i) {
            const int blockStartIdx = blockedPermute[i] * permuteBlockSize;
            const int blockEndIndx = Min(blockStartIdx + permuteBlockSize, data.LearnSampleCount);
            for (int j = blockStartIdx; j < blockEndIndx; ++j) {
                fold->LearnPermutation[currentIdx + j - blockStartIdx] = j;
            }
            currentIdx += blockEndIndx - blockStartIdx;
        }
        fold->PermutationBlockSize = permuteBlockSize;
    }
}

TFold BuildDynamicFold(
    const TTrainData& data,
    const TVector<TTargetClassifier>& targetClassifiers,
    bool shuffle,
    int permuteBlockSize,
    int approxDimension,
    double multiplier,
    bool storeExpApproxes,
    TRestorableFastRng64& rand
) {
    TFold ff;
    ff.SampleWeights.resize(data.LearnSampleCount, 1);
    ff.LearnPermutation.resize(data.LearnSampleCount);

    std::iota(ff.LearnPermutation.begin(), ff.LearnPermutation.end(), 0);
    if (shuffle) {
        ShuffleData(data, permuteBlockSize, rand, &ff);
    } else {
        ff.PermutationBlockSize = data.LearnSampleCount;
    }

    ff.AssignTarget(data.Target, targetClassifiers);

    if (!data.Weights.empty()) {
        ff.AssignPermuted(data.Weights, &ff.LearnWeights);
    }

    if (!data.QueryId.empty()) {
        ff.AssignPermuted(data.QueryId, &ff.LearnQueryId);
        ff.LearnQuerySize = data.QuerySize;
    }
    TVector<int> queriesFinishIndex = CalcQueriesFinishIndex(ff.LearnQueryId);
    ff.EffectiveDocCount = data.LearnSampleCount;

    TVector<int> invertPermutation = InvertPermutation(ff.LearnPermutation);

    int leftPartLen = SelectMinBatchSize(data.LearnSampleCount, queriesFinishIndex);
    while (ff.BodyTailArr.empty() || leftPartLen < data.LearnSampleCount) {
        TFold::TBodyTail bt;
        bt.BodyFinish = leftPartLen;
        bt.TailFinish = Min(SelectTailSize(leftPartLen, multiplier, queriesFinishIndex), ff.LearnPermutation.ysize() + 0.);
        bt.Approx.resize(approxDimension, TVector<double>(bt.TailFinish, GetNeutralApprox(storeExpApproxes)));
        if (!data.Baseline.empty()) {
            InitFromBaseline(leftPartLen, bt.TailFinish, data.Baseline, ff.LearnPermutation, storeExpApproxes, &bt.Approx);
        }
        bt.Derivatives.resize(approxDimension, TVector<double>(bt.TailFinish));
        bt.WeightedDer.resize(approxDimension, TVector<double>(bt.TailFinish));
        ff.AssignCompetitors(data.Pairs, invertPermutation, &bt);
        ff.BodyTailArr.emplace_back(std::move(bt));
        leftPartLen = bt.TailFinish;
    }
    return ff;
}

TFold BuildPlainFold(
    const TTrainData& data,
    const TVector<TTargetClassifier>& targetClassifiers,
    bool shuffle,
    int permuteBlockSize,
    int approxDimension,
    bool storeExpApproxes,
    TRestorableFastRng64& rand
) {
    TFold ff;
    ff.SampleWeights.resize(data.LearnSampleCount, 1);
    ff.LearnPermutation.resize(data.LearnSampleCount);

    std::iota(ff.LearnPermutation.begin(), ff.LearnPermutation.end(), 0);
    if (shuffle) {
        ShuffleData(data, permuteBlockSize, rand, &ff);
    } else {
        ff.PermutationBlockSize = data.LearnSampleCount;
    }

    ff.AssignTarget(data.Target, targetClassifiers);

    if (!data.Weights.empty()) {
        ff.AssignPermuted(data.Weights, &ff.LearnWeights);
    }

    if (!data.QueryId.empty()) {
        ff.AssignPermuted(data.QueryId, &ff.LearnQueryId);
        ff.LearnQuerySize = data.QuerySize;
    }

    ff.EffectiveDocCount = data.GetSampleCount();

    TVector<int> invertPermutation = InvertPermutation(ff.LearnPermutation);

    TFold::TBodyTail bt;
    bt.BodyFinish = data.LearnSampleCount;
    bt.TailFinish = data.LearnSampleCount;
    bt.Approx.resize(approxDimension, TVector<double>(data.GetSampleCount(), GetNeutralApprox(storeExpApproxes)));
    bt.Derivatives.resize(approxDimension, TVector<double>(data.GetSampleCount()));
    bt.WeightedDer.resize(approxDimension, TVector<double>(data.GetSampleCount()));
    if (!data.Baseline.empty()) {
        InitFromBaseline(0, data.GetSampleCount(), data.Baseline, ff.LearnPermutation, storeExpApproxes, &bt.Approx);
    }
    ff.AssignCompetitors(data.Pairs, invertPermutation, &bt);
    ff.BodyTailArr.emplace_back(std::move(bt));
    return ff;
}


void TFold::DropEmptyCTRs() {
    TVector<TProjection> emptyProjections;
    for (auto& projCtr : OnlineSingleCtrs) {
        if (projCtr.second.Feature.empty()) {
            emptyProjections.emplace_back(projCtr.first);
        }
    }
    for (auto& projCtr : OnlineCTR) {
        if (projCtr.second.Feature.empty()) {
            emptyProjections.emplace_back(projCtr.first);
        }
    }
    for (const auto& proj : emptyProjections) {
        GetCtrs(proj).erase(proj);
    }
}

void TFold::AssignTarget(const TVector<float>& target, const TVector<TTargetClassifier>& targetClassifiers) {
    AssignPermuted(target, &LearnTarget);
    int learnSampleCount = LearnPermutation.ysize();

    int ctrCount = targetClassifiers.ysize();
    LearnTargetClass.assign(ctrCount, TVector<int>(learnSampleCount));
    TargetClassesCount.resize(ctrCount);
    for (int ctrIdx = 0; ctrIdx < ctrCount; ++ctrIdx) {
        for (int z = 0; z < learnSampleCount; ++z) {
            LearnTargetClass[ctrIdx][z] = targetClassifiers[ctrIdx].GetTargetClass(LearnTarget[z]);
        }
        TargetClassesCount[ctrIdx] = targetClassifiers[ctrIdx].GetClassesCount();
    }
}


void TFold::AssignCompetitors(const TVector<TPair>& pairs, const TVector<int>& invertPermutation, TBodyTail* bt) {
    int learnSampleCount = LearnPermutation.ysize();
    int bodyFinish = bt->BodyFinish;
    int tailFinish = bt->TailFinish;
    TVector<TVector<TCompetitor>>& competitors = bt->Competitors;
    competitors.resize(tailFinish);
    for (const auto& pair : pairs) {
        if (pair.WinnerId >= learnSampleCount || pair.LoserId >= learnSampleCount) {
            continue;
        }

        int winnerId = invertPermutation[pair.WinnerId];
        int loserId = invertPermutation[pair.LoserId];

        if (winnerId >= tailFinish || loserId >= tailFinish) {
            continue;
        }

        float weight = pair.Weight;

        if (winnerId < bodyFinish || winnerId > loserId) {
            competitors[winnerId].emplace_back(loserId, weight);
        }
        if (loserId < bodyFinish || loserId > winnerId) {
            competitors[loserId].emplace_back(winnerId, -weight);
        }
    }
}

void TFold::SaveApproxes(IOutputStream* s) const {
    const ui64 bodyTailCount = BodyTailArr.size();
    ::Save(s, bodyTailCount);
    for (ui64 i = 0; i < bodyTailCount; ++i) {
        ::Save(s, BodyTailArr[i].Approx);
    }
}

void TFold::LoadApproxes(IInputStream* s) {
    ui64 bodyTailCount;
    ::Load(s, bodyTailCount);
    CB_ENSURE(bodyTailCount == BodyTailArr.size());
    for (ui64 i = 0; i < bodyTailCount; ++i) {
        ::Load(s, BodyTailArr[i].Approx);
    }
}
