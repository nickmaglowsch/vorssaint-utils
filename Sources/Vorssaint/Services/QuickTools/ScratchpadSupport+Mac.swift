// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Vorssaint
//
// Split out of ScratchpadSupport.swift by WP-11. The Markdown preview is the
// one platform corner of that file: it renders through
// `AttributedString(markdown:options:)` and reads the block structure back out
// of `PresentationIntent`, and neither the Markdown initializer nor
// `PresentationIntent` exists in swift-corelibs-foundation — the Linux build
// reported `extra argument 'options' in call` and `cannot find type
// 'PresentationIntent' in scope` (run 34661825794). Everything else in the
// file — retention, the document model, the tab shortcuts, the naming and
// export rules — is pure and moved to VorssaintCore.
//
// The Mac build has no module boundary between the two halves (PLAN.md § 5),
// so this is the same code in the same module it always was; the Linux
// Scratchpad renders Markdown through its own toolkit (WP-A9).

import Foundation

struct ScratchpadMarkdownBlock {
    enum Kind: Equatable {
        case paragraph
        case heading(Int)
        case unorderedListItem(depth: Int)
        case orderedListItem(ordinal: Int, depth: Int)
        case quote(depth: Int)
        case code
        case thematicBreak
    }

    let kind: Kind
    let containerID: Int?
    let text: AttributedString
}

extension ScratchpadSupport {
    /// Rendering is on demand and never changes the stored plain text. Native
    /// presentation intents keep headings, lists and quotes semantic without a
    /// second Markdown parser or any work while the preview is closed.
    static func markdownPreview(_ text: String) -> [ScratchpadMarkdownBlock] {
        guard !text.isEmpty else { return [] }
        let options = AttributedString.MarkdownParsingOptions(
            interpretedSyntax: .full,
            failurePolicy: .returnPartiallyParsedIfPossible)
        guard let parsed = try? AttributedString(markdown: text, options: options) else {
            return [ScratchpadMarkdownBlock(kind: .paragraph,
                                            containerID: nil,
                                            text: AttributedString(text))]
        }

        var blocks: [ScratchpadMarkdownBlock] = []
        var currentID: Int?
        var currentKind = ScratchpadMarkdownBlock.Kind.paragraph
        var currentContainerID: Int?
        var currentText = AttributedString()

        func finishBlock() {
            guard currentID != nil else { return }
            if currentKind == .code {
                while currentText.characters.last?.isNewline == true {
                    currentText.characters.removeLast()
                }
            }
            blocks.append(ScratchpadMarkdownBlock(kind: currentKind,
                                                  containerID: currentContainerID,
                                                  text: currentText))
            currentText = AttributedString()
        }

        for run in parsed.runs {
            let components = run.presentationIntent?.components ?? []
            let blockID = components.first?.identity ?? 0
            if blockID != currentID {
                finishBlock()
                currentID = blockID
                let presentation = markdownBlockPresentation(components)
                currentKind = presentation.kind
                currentContainerID = presentation.containerID
            }
            currentText.append(AttributedString(parsed[run.range]))
        }
        finishBlock()

        return blocks.isEmpty
            ? [ScratchpadMarkdownBlock(kind: .paragraph,
                                       containerID: nil,
                                       text: AttributedString(text))]
            : blocks
    }

    private static func markdownBlockPresentation(
        _ components: [PresentationIntent.IntentType]
    ) -> (kind: ScratchpadMarkdownBlock.Kind, containerID: Int?) {
        var listDepth = 0
        var listIsOrdered: Bool?
        var listOrdinal = 1
        var quoteDepth = 0
        var containerID: Int?

        for component in components {
            switch component.kind {
            case .header(let level):
                return (.heading(level), nil)
            case .codeBlock:
                return (.code, nil)
            case .thematicBreak:
                return (.thematicBreak, nil)
            case .listItem(let ordinal):
                if listDepth == 0 { listOrdinal = ordinal }
            case .orderedList:
                listDepth += 1
                if listIsOrdered == nil { listIsOrdered = true }
                containerID = component.identity
            case .unorderedList:
                listDepth += 1
                if listIsOrdered == nil { listIsOrdered = false }
                containerID = component.identity
            case .blockQuote:
                quoteDepth += 1
                containerID = component.identity
            case .paragraph, .table, .tableHeaderRow, .tableRow, .tableCell:
                break
            @unknown default:
                break
            }
        }

        if listIsOrdered == true {
            return (.orderedListItem(ordinal: listOrdinal, depth: max(1, listDepth)), containerID)
        }
        if listIsOrdered == false {
            return (.unorderedListItem(depth: max(1, listDepth)), containerID)
        }
        if quoteDepth > 0 { return (.quote(depth: quoteDepth), containerID) }
        return (.paragraph, nil)
    }
}
