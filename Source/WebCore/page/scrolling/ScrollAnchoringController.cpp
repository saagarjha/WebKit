/*
* Copyright (C) 2022 Apple Inc. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
* THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
* PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
* THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"
#include "ScrollAnchoringController.h"
#include "ElementChildIteratorInlines.h"
#include "ElementIterator.h"
#include "HTMLHtmlElement.h"
#include "LocalFrameView.h"
#include "Logging.h"
#include "RenderBox.h"
#include "RenderLayerScrollableArea.h"
#include "RenderObjectInlines.h"
#include "RenderView.h"
#include "TypedElementDescendantIteratorInlines.h"
#include <wtf/text/TextStream.h>

namespace WebCore {

ScrollAnchoringController::ScrollAnchoringController(ScrollableArea& owningScroller)
    : m_owningScrollableArea(owningScroller)
{ }

LocalFrameView& ScrollAnchoringController::frameView()
{
    if (is<RenderLayerScrollableArea>(m_owningScrollableArea))
        return downcast<RenderLayerScrollableArea>(m_owningScrollableArea).layer().renderer().view().frameView();
    return downcast<LocalFrameView>(downcast<ScrollView>(m_owningScrollableArea));
}

void ScrollAnchoringController::invalidateAnchorElement()
{
    if (m_midUpdatingScrollPositionForAnchorElement)
        return;
    LOG_WITH_STREAM(ScrollAnchoring, stream << "ScrollAnchoringController::invalidateAnchorElement() invalidating anchor for frame: " << frameView() << " for scroller: " << m_owningScrollableArea);

    m_anchorElement = nullptr;
    m_lastOffsetForAnchorElement = { };
    m_isQueuedForScrollPositionUpdate = false;
    frameView().queueScrollableAreaForScrollAnchoringUpdate(m_owningScrollableArea);
}

static IntRect boundingRectForScrollableArea(ScrollableArea& scrollableArea)
{
    if (is<RenderLayerScrollableArea>(scrollableArea))
        return downcast<RenderLayerScrollableArea>(scrollableArea).layer().renderer().absoluteBoundingBoxRect();

    return IntRect(downcast<LocalFrameView>(downcast<ScrollView>(scrollableArea)).layoutViewportRect());
}

static Element* elementForScrollableArea(ScrollableArea& scrollableArea)
{
    if (is<RenderLayerScrollableArea>(scrollableArea))
        return downcast<RenderLayerScrollableArea>(scrollableArea).layer().renderer().element();
    if (auto* document = downcast<LocalFrameView>(downcast<ScrollView>(scrollableArea)).frame().document())
        return document->documentElement();
    return nullptr;
}

FloatPoint ScrollAnchoringController::computeOffsetFromOwningScroller(RenderObject& candidate)
{
    // TODO: investigate this for zoom/rtl
    return FloatPoint(candidate.absoluteBoundingBoxRect().location() - boundingRectForScrollableArea(m_owningScrollableArea).location());
}

static RefPtr<Element> anchorElementForPriorityCandidate(Element* element)
{
    while (element) {
        if (auto renderer = element->renderer()) {
            if (!renderer->isAnonymousBlock() && (!renderer->isInline() || renderer->isAtomicInlineLevelBox()))
                return element;
        }
        element = element->parentElement();
    }
    return nullptr;
}

bool ScrollAnchoringController::didFindPriorityCandidate(Document& document)
{
    auto viablePriorityCandidateForElement = [this](Element* element) -> RefPtr<Element> {
        RefPtr candidateElement = anchorElementForPriorityCandidate(element);
        if (!candidateElement)
            return nullptr;

        LOG_WITH_STREAM(ScrollAnchoring, stream << "ScrollAnchoringController::viablePriorityCandidateForElement()");
        RefPtr iterElement = candidateElement;

        while (iterElement && iterElement.get() != elementForScrollableArea(m_owningScrollableArea)) {
            if (auto renderer = element->renderer()) {
                if (renderer->style().overflowAnchor() == OverflowAnchor::None)
                    return nullptr;
            }
            iterElement = iterElement->parentElement();
        }
        return candidateElement;
    };

    // TODO: need to check if focused element is text editable
    // TODO: need to figure out how to get element that is the current find-in-page element (look into FindController)
    if (RefPtr priorityCandidate = viablePriorityCandidateForElement(document.focusedElement())) {
        m_anchorElement = priorityCandidate;
        m_lastOffsetForAnchorElement = computeOffsetFromOwningScroller(*m_anchorElement->renderer());
        return true;
    }
    return false;
}

CandidateExaminationResult ScrollAnchoringController::examineCandidate(Element& element)
{
    if (elementForScrollableArea(m_owningScrollableArea) && elementForScrollableArea(m_owningScrollableArea)->identifier() == element.identifier())
        return CandidateExaminationResult::Skip;

    auto containingRect = boundingRectForScrollableArea(m_owningScrollableArea);
    auto* document = frameView().frame().document();

    if (auto renderer = element.renderer()) {
        // TODO: we need to think about position: absolute
        // TODO: figure out how to get scrollable area for renderer to check if it is maintaining scroll anchor
        if (renderer->style().overflowAnchor() == OverflowAnchor::None || renderer->isStickilyPositioned() || renderer->isFixedPositioned() || renderer->isPseudoElement() || renderer->isAnonymousBlock())
            return CandidateExaminationResult::Exclude;
        if (&element == document->bodyOrFrameset() || is<HTMLHtmlElement>(&element) || (renderer->isInline() && !renderer->isAtomicInlineLevelBox()))
            return CandidateExaminationResult::Skip;
        auto boxRect = renderer->absoluteBoundingBoxRect();
        if (!boxRect.width() || !boxRect.height())
            return CandidateExaminationResult::Skip;
        if (containingRect.contains(boxRect))
            return CandidateExaminationResult::Select;
        auto isScrollingNode = false;
        if (auto* renderBox = dynamicDowncast<RenderBox>(renderer))
            isScrollingNode = renderBox->hasPotentiallyScrollableOverflow();
        if (containingRect.intersects(boxRect))
            return isScrollingNode ? CandidateExaminationResult::Select : CandidateExaminationResult::Descend;
        if (isScrollingNode)
            return CandidateExaminationResult::Exclude;
    }
    return CandidateExaminationResult::Skip;
}

#if !LOG_DISABLED
static TextStream& operator<<(TextStream& ts, CandidateExaminationResult result)
{
    switch (result) {
    case CandidateExaminationResult::Exclude:
        ts << "Exclude";
        break;
    case CandidateExaminationResult::Select:
        ts << "Select";
        break;
    case CandidateExaminationResult::Descend:
        ts << "Descend";
        break;
    case CandidateExaminationResult::Skip:
        ts << "Skip";
        break;
    }
    return ts;
}
#endif

Element* ScrollAnchoringController::findAnchorElementRecursive(Element* element)
{
    if (!element)
        return nullptr;

    auto result = examineCandidate(*element);
    LOG_WITH_STREAM(ScrollAnchoring, stream << "ScrollAnchoringController::findAnchorElementRecursive() element: "<< *element<<" examination result: " << result);

    switch (result) {
    case CandidateExaminationResult::Select:
        return element;
    case CandidateExaminationResult::Exclude:
        return nullptr;
    case CandidateExaminationResult::Skip:
    case CandidateExaminationResult::Descend: {
        for (auto& child : childrenOfType<Element>(*element)) {
            if (auto* anchorElement = findAnchorElementRecursive(&child))
                return anchorElement;
        }
        break;
    }
    }
    if (result == CandidateExaminationResult::Skip)
        return nullptr;
    return element;
}

void ScrollAnchoringController::chooseAnchorElement(Document& document)
{
    LOG_WITH_STREAM(ScrollAnchoring, stream << "ScrollAnchoringController::chooseAnchorElement() starting findAnchorElementRecursive: ");

    if (didFindPriorityCandidate(document))
        return;

    RefPtr<Element> anchorElement;

    if (!m_anchorElement) {
        anchorElement = findAnchorElementRecursive(elementForScrollableArea(m_owningScrollableArea));
        if (!anchorElement)
            return;
    }

    m_anchorElement = anchorElement;
    m_lastOffsetForAnchorElement = computeOffsetFromOwningScroller(*m_anchorElement->renderer());
    LOG_WITH_STREAM(ScrollAnchoring, stream << "ScrollAnchoringController::chooseAnchorElement() found anchor node: " << *anchorElement << " offset: " << computeOffsetFromOwningScroller(*m_anchorElement->renderer()));
}

void ScrollAnchoringController::updateAnchorElement()
{
    if (m_owningScrollableArea.scrollPosition().isZero() || m_isQueuedForScrollPositionUpdate || frameView().layoutContext().layoutPhase() != LocalFrameViewLayoutContext::LayoutPhase::OutsideLayout)
        return;

    RefPtr document = frameView().frame().document();
    if (!document)
        return;

    if (m_anchorElement && !m_anchorElement->renderer())
        invalidateAnchorElement();

    if (!m_anchorElement) {
        chooseAnchorElement(*document);
        if (!m_anchorElement)
            return;
    }
    m_isQueuedForScrollPositionUpdate = true;
    frameView().queueScrollableAreaForScrollAnchoringUpdate(m_owningScrollableArea);
}

void ScrollAnchoringController::adjustScrollPositionForAnchoring()
{
    SetForScope midUpdatingScrollPositionForAnchorElement(m_midUpdatingScrollPositionForAnchorElement, true);
    auto queued = std::exchange(m_isQueuedForScrollPositionUpdate, false);
    if (!m_anchorElement || !queued)
        return;
    auto renderBox = m_anchorElement->renderer();
    if (!renderBox) {
        invalidateAnchorElement();
        return;
    }
    FloatSize adjustment = computeOffsetFromOwningScroller(*renderBox) - m_lastOffsetForAnchorElement;
    if (!adjustment.isZero()) {
        auto newScrollPosition = m_owningScrollableArea.scrollPosition() + IntPoint(adjustment.width(), adjustment.height());
        LOG_WITH_STREAM(ScrollAnchoring, stream << "ScrollAnchoringController::updateScrollPosition() for frame: " << frameView() << " for scroller: " << m_owningScrollableArea << " adjusting from: " << m_owningScrollableArea.scrollPosition() << " to: " << newScrollPosition);
        auto options = ScrollPositionChangeOptions::createProgrammatic();
        options.originalScrollDelta = adjustment;
        auto oldScrollType = m_owningScrollableArea.currentScrollType();
        m_owningScrollableArea.setCurrentScrollType(ScrollType::Programmatic);
        if (!m_owningScrollableArea.requestScrollToPosition(newScrollPosition, options))
            m_owningScrollableArea.scrollToPositionWithoutAnimation(newScrollPosition);
        m_owningScrollableArea.setCurrentScrollType(oldScrollType);
    }
}

} // namespace WebCore
