/*
   This file is part of the clang-lazy static checker.

  Copyright (C) 2015 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Sérgio Martins <sergio.martins@kdab.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

  As a special exception, permission is given to link this program
  with any edition of Qt, and distribute the resulting executable,
  without including the source code for Qt in the source distribution.
*/

#include "qt4-qstring-from-array.h"
#include "Utils.h"
#include "checkmanager.h"
#include "StringUtils.h"
#include "FixItUtils.h"

#include <clang/AST/AST.h>
#include <clang/Lex/Lexer.h>

using namespace clang;
using namespace std;

enum FixIt {
    FixItNone,
    FixItToFromLatin1
};

Qt4_QStringFromArray::Qt4_QStringFromArray(const std::string &name, const clang::CompilerInstance &ci)
    : CheckBase(name, ci)
{
}

static bool isInterestingParam(ParmVarDecl *param, bool &is_char_array, bool &is_byte_array)
{
    is_char_array = false;
    is_byte_array = false;
    const string typeStr = param->getType().getAsString();
    if (typeStr == "const class QByteArray &") {
        is_byte_array = true;
    } else if (typeStr == "const char *") {// We only want bytearray and const char*
        is_char_array = true;
    }

    return is_char_array || is_byte_array;
}

static bool isInterestingCtorCall(CXXConstructorDecl *ctor, bool &is_char_array, bool &is_byte_array)
{
    is_char_array = false;
    is_byte_array = false;
    if (!ctor || !isOfClass(ctor, "QString"))
        return false;

    for (auto it = ctor->param_begin(), end = ctor->param_end(); it != end; ++it) {
        if (isInterestingParam(*it, is_char_array, is_byte_array))
            break;

        return false;
    }

    return is_char_array || is_byte_array;
}

static bool isInterestingMethod(const string &methodName)
{
    static const vector<string> methods = { "append", "prepend", "operator=", "operator==", "operator!=", "operator<", "operator<=", "operator>", "operator>=", "operator+=" };
    return std::find(methods.cbegin(), methods.cend(), methodName) != methods.cend();
}

static bool isInterestingMethodCall(CXXMethodDecl *method, string &methodName, bool &is_char_array, bool &is_byte_array)
{
    is_char_array = false;
    is_byte_array = false;
    if (!method)
        return false;

    if (method->getParent()->getNameAsString() != "QString" || method->getNumParams() != 1)
        return false;

    methodName = method->getNameAsString();
    if (!isInterestingMethod(methodName))
        return false;

    if (!isInterestingParam(method->getParamDecl(0), is_char_array, is_byte_array))
        return false;

    return true;
}

static bool isInterestingOperatorCall(CXXOperatorCallExpr *op, string &operatorName, bool &is_char_array, bool &is_byte_array)
{
    is_char_array = false;
    is_byte_array = false;
    FunctionDecl *func = op->getDirectCallee();
    if (!func)
        return false;

    return isInterestingMethodCall(dyn_cast<CXXMethodDecl>(func), operatorName, is_char_array, is_byte_array);
}

void Qt4_QStringFromArray::VisitStmt(clang::Stmt *stm)
{
    CXXConstructExpr *ctorExpr = dyn_cast<CXXConstructExpr>(stm);
    CXXOperatorCallExpr *operatorCall = dyn_cast<CXXOperatorCallExpr>(stm);
    CXXMemberCallExpr *memberCall = dyn_cast<CXXMemberCallExpr>(stm);
    if (!ctorExpr && !operatorCall && !memberCall)
        return;

    vector<FixItHint> fixits;
    bool is_char_array = false;
    bool is_byte_array = false;
    string methodName;
    string message;

    if (ctorExpr) {
        CXXConstructorDecl *ctorDecl = ctorExpr->getConstructor();

        if (!isInterestingCtorCall(ctorDecl, is_char_array, is_byte_array))
            return;

        fixits = fixCtorCall(ctorExpr);
        if (is_char_array) {
            message = "QString(const char *) ctor being called";
        } else {
            message = "QString(QByteArray) ctor being called";
        }
    } else if (operatorCall) {
        if (!isInterestingOperatorCall(operatorCall, /*by-ref*/methodName, is_char_array, is_byte_array))
            return;

        fixits = fixOperatorCall(operatorCall);
    } else if (memberCall) {
        if (!isInterestingMethodCall(memberCall->getMethodDecl(), /*by-ref*/methodName, is_char_array, is_byte_array))
            return;

        fixits = fixMethodCallCall(memberCall);
    } else {
        return;
    }

    if (operatorCall || memberCall) {
        if (is_char_array) {
            message = "QString::" + methodName + "(const char *) being called";
        } else {
            message = "QString::" + methodName + "(QByteArray) being called";
        }
    }

    emitWarning(stm->getLocStart(), message, fixits);
}

std::vector<FixItHint> Qt4_QStringFromArray::fixCtorCall(CXXConstructExpr *ctorExpr)
{
    Stmt *parent = HierarchyUtils::parent(m_parentMap, ctorExpr); // CXXBindTemporaryExpr
    Stmt *grandParent = HierarchyUtils::parent(m_parentMap, parent); //CXXFunctionalCastExpr

    if (parent && grandParent && isa<CXXBindTemporaryExpr>(parent) && isa<CXXFunctionalCastExpr>(grandParent)) {
        return fixitReplaceWithFromLatin1(ctorExpr);
    } else {
        return fixitInsertFromLatin1(ctorExpr);
    }
}

std::vector<FixItHint> Qt4_QStringFromArray::fixOperatorCall(CXXOperatorCallExpr *op)
{
    vector<FixItHint> fixits;
    if (op->getNumArgs() == 2) {
        Expr *e = op->getArg(1);
        SourceLocation start = e->getLocStart();
        SourceLocation end = Lexer::getLocForEndOfToken(FixItUtils::biggestSourceLocationInStmt(m_ci.getSourceManager(), e), 0, m_ci.getSourceManager(), m_ci.getLangOpts());

        SourceRange range = { start, end };
        if (range.isInvalid()) {
            emitWarning(op->getLocStart(), "internal error");
            return {};
        }

        FixItUtils::insertParentMethodCall("QString::fromLatin1", {start, end}, /*by-ref*/fixits);

    } else {
        emitWarning(op->getLocStart(), "internal error");
    }


    return fixits;
}

std::vector<FixItHint> Qt4_QStringFromArray::fixMethodCallCall(clang::CXXMemberCallExpr *memberExpr)
{
    vector<FixItHint> fixits;

    if (memberExpr->getNumArgs() == 1) {
        Expr *e = *(memberExpr->arg_begin());
        SourceLocation start = e->getLocStart();
        SourceLocation end = Lexer::getLocForEndOfToken(FixItUtils::biggestSourceLocationInStmt(m_ci.getSourceManager(), e), 0, m_ci.getSourceManager(), m_ci.getLangOpts());

        SourceRange range = { start, end };
        if (range.isInvalid()) {
            emitWarning(memberExpr->getLocStart(), "internal error");
            return {};
        }

        FixItUtils::insertParentMethodCall("QString::fromLatin1", {start, end}, /*by-ref*/fixits);
    } else {
        emitWarning(memberExpr->getLocStart(), "internal error");
    }


    return fixits;
}

std::vector<FixItHint> Qt4_QStringFromArray::fixitReplaceWithFromLatin1(CXXConstructExpr *ctorExpr)
{
    const string replacement = "QString::fromLatin1";
    const string replacee = "QString";
    vector<FixItHint> fixits;

    const auto &sm = m_ci.getSourceManager();
    SourceLocation rangeStart = ctorExpr->getLocStart();
    SourceLocation rangeEnd = Lexer::getLocForEndOfToken(rangeStart, -1, sm, m_ci.getLangOpts());

    if (rangeEnd.isInvalid()) {
        // Fallback. Have seen a case in the wild where the above would fail, it's very rare
        rangeEnd = rangeStart.getLocWithOffset(replacee.size() - 2);
        if (rangeEnd.isInvalid()) {
            StringUtils::printLocation(sm, rangeStart);
            StringUtils::printLocation(sm, rangeEnd);
            StringUtils::printLocation(sm, Lexer::getLocForEndOfToken(rangeStart, 0, sm, m_ci.getLangOpts()));
            queueManualFixitWarning(ctorExpr->getLocStart(), FixItToFromLatin1);
            return {};
        }
    }

    fixits.push_back(FixItHint::CreateReplacement(SourceRange(rangeStart, rangeEnd), replacement));
    return fixits;
}

std::vector<FixItHint> Qt4_QStringFromArray::fixitInsertFromLatin1(CXXConstructExpr *ctorExpr)
{
    vector<FixItHint> fixits;
    SourceRange range;

    Expr *arg = *(ctorExpr->arg_begin());
    range.setBegin(arg->getLocStart());
    range.setEnd(Lexer::getLocForEndOfToken(FixItUtils::biggestSourceLocationInStmt(m_ci.getSourceManager(), ctorExpr), 0, m_ci.getSourceManager(), m_ci.getLangOpts()));
    if (range.isInvalid()) {
        emitWarning(ctorExpr->getLocStart(), "Internal error");
        return {};
    }

    FixItUtils::insertParentMethodCall("QString::fromLatin1", range, fixits);

    return fixits;
}

const char *const s_checkName = "qt4-qstring-from-array";
REGISTER_CHECK_WITH_FLAGS(s_checkName, Qt4_QStringFromArray, HiddenCheckLevel)
REGISTER_FIXIT(FixItToFromLatin1, "fix-qt4-qstring-from-array", s_checkName)
