// Copyright 2018 CommScope, Inc.

/*
  Copyright (C) 2011 Joseph A. Adams (joeyadams3.14159@gmail.com)
  All rights reserved.

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/**
 * \file json.c
 *
 * Implements a JSON parser and serializer (from the ccan/json module)
 *
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

//------------------------------------------------------------------------------------
// String buffer used to build up the text output by this module.
// The buffer always has space for a NULL terminator after the character pointed to by 'end'
typedef struct
{
    char *start;            // Pointer to the start of the allocated buffer
    char *cur;              // Pointer to the next free character in the buffer
    char *end;              // Pointer to the last character which may be written without growing the buffer
} SB;

//------------------------------------------------------------------------------------
// Type for Unicode codepoints. We need our own because wchar_t might be 16 bits
typedef uint32_t uchar_t;

//------------------------------------------------------------------------------------
// Size of the buffer passed to json_check() to receive the description of any problem found
#define ERR_MSG_LEN 256

//------------------------------------------------------------------------------------
// Maximum number of bytes in a UTF-8 encoded character
#define MAX_UTF8_CHAR_LEN 4

//------------------------------------------------------------------------------------
// Maximum number of bytes written when emitting a single character of a JSON string.
// This is enough space to write up to two '\uXXXX' escapes and two quotation marks
#define MAX_EMITTED_CHAR_LEN 14

//------------------------------------------------------------------------------------
// Number of characters in the hexadecimal form of a UTF-16 code unit (ie the 'XXXX' in '\uXXXX')
#define HEX16_LEN 4

//------------------------------------------------------------------------------------
// Character classification tests
#define IS_SPACE(c) (((c) == '\t') || ((c) == '\n') || ((c) == '\r') || ((c) == ' '))
#define IS_DIGIT(c) (((c) >= '0') && ((c) <= '9'))

//------------------------------------------------------------------------------------
// Forward declarations. Note these are not static, because we need them in the symbol table for USP_LOG_Callstack() to show them
bool ParseValue(const char **sp, JsonNode **out);
bool ParseArray(const char **sp, JsonNode **out);
bool ParseObject(const char **sp, JsonNode **out);
bool ParseString(const char **sp, char **out);
int ParseString_Char(const char **sp, char *b);
int ParseString_Escape(const char **sp, char *b);
int ParseString_Unicode(const char **sp, char *b);
bool ParseNumber(const char **sp, double *out);
bool ParseHex16(const char **sp, uint16_t *out);
int HexToNibble(char c);
bool ExpectLiteral(const char **sp, const char *str);
void SkipSpace(const char **sp);
void EmitValue(SB *out, const JsonNode *node);
void EmitValue_Indented(SB *out, const JsonNode *node, const char *space, int indent_level);
void EmitArray(SB *out, const JsonNode *array);
void EmitArray_Indented(SB *out, const JsonNode *array, const char *space, int indent_level);
void EmitObject(SB *out, const JsonNode *object);
void EmitObject_Indented(SB *out, const JsonNode *object, const char *space, int indent_level);
void EmitString(SB *out, const char *str);
int EmitString_Escape(char *b, unsigned char c);
int EmitString_Char(char *b, const char **sp, bool escape_unicode);
void EmitNumber(SB *out, double num);
void EmitIndent(SB *out, const char *space, int indent_level);
int WriteHex16(char *out, uint16_t val);
JsonNode *MakeNode(JsonTag tag);
JsonNode *MakeString(char *s);
void AppendNode(JsonNode *parent, JsonNode *child);
void PrependNode(JsonNode *parent, JsonNode *child);
void AppendMember(JsonNode *object, char *key, JsonNode *value);
bool CheckChildren(const JsonNode *node, char *errmsg);
bool IsValidTag(unsigned int tag);
bool IsValidNumber(const char *num);
bool ReportProblem(char *errmsg, const char *fmt, ...);
bool Utf8ValidateString(const char *s);
int Utf8ValidateChar(const char *s);
int Utf8ReadChar(const char *s, uchar_t *out);
int Utf8WriteChar(uchar_t unicode, char *out);
bool FromSurrogatePair(uint16_t uc, uint16_t lc, uchar_t *unicode);
void ToSurrogatePair(uchar_t unicode, uint16_t *uc, uint16_t *lc);
void SbInit(SB *sb);
void SbNeed(SB *sb, int need);
void SbGrow(SB *sb, int need);
void SbPut(SB *sb, const char *bytes, int count);
void SbPutChar(SB *sb, char c);
void SbPuts(SB *sb, const char *str);
char *SbFinish(SB *sb);
void SbFree(SB *sb);
char *StrDup(const char *str);
void OutOfMemory(void);

#if 1
// Copyright (C) 2022, Broadband Forum
// Copyright (C) 2022  CommScope, Inc
void EmitNumber_LongLong(SB *out, long long num);
void EmitNumber_UnsignedLongLong(SB *out, unsigned long long num);
#endif

/*********************************************************************//**
**
** json_decode
**
** Parses the specified JSON text, creating a tree of JsonNodes representing it
**
** \param   json - pointer to NULL terminated string containing the JSON text to parse
**
** \return  pointer to the root node of the parsed tree, or NULL if the text was not valid JSON
**          NOTE: Ownership of the returned tree passes to the caller, which must free it using json_delete()
**
**************************************************************************/
JsonNode *json_decode(const char *json)
{
    const char *s;
    JsonNode *ret;
    bool result;

    s = json;
    SkipSpace(&s);
    result = ParseValue(&s, &ret);
    if (result == false)
    {
        return NULL;
    }

    // Exit if there is any trailing text after the JSON value
    SkipSpace(&s);
    if (s[0] != '\0')
    {
        json_delete(ret);
        return NULL;
    }

    return ret;
}

/*********************************************************************//**
**
** json_validate
**
** Determines whether the specified text is valid JSON
**
** \param   json - pointer to NULL terminated string containing the JSON text to validate
**
** \return  true if the text is valid JSON, false otherwise
**
**************************************************************************/
bool json_validate(const char *json)
{
    const char *s;
    bool result;

    s = json;
    SkipSpace(&s);
    result = ParseValue(&s, NULL);
    if (result == false)
    {
        return false;
    }

    // Exit if there is any trailing text after the JSON value
    SkipSpace(&s);
    if (s[0] != '\0')
    {
        return false;
    }

    return true;
}

/*********************************************************************//**
**
** json_encode
**
** Serializes the specified tree of JsonNodes into compact JSON text
**
** \param   node - pointer to the root node of the tree to serialize
**
** \return  pointer to dynamically allocated NULL terminated string containing the JSON text
**          NOTE: Ownership of the returned string passes to the caller
**
**************************************************************************/
char *json_encode(const JsonNode *node)
{
    return json_stringify(node, NULL);
}

/*********************************************************************//**
**
** json_encode_string
**
** Serializes the specified string into a quoted and escaped JSON string
**
** \param   str - pointer to NULL terminated string to serialize
**
** \return  pointer to dynamically allocated NULL terminated string containing the JSON text
**          NOTE: Ownership of the returned string passes to the caller
**
**************************************************************************/
char *json_encode_string(const char *str)
{
    SB sb;

    SbInit(&sb);
    EmitString(&sb, str);

    return SbFinish(&sb);
}

/*********************************************************************//**
**
** json_stringify
**
** Serializes the specified tree of JsonNodes into JSON text
**
** \param   node - pointer to the root node of the tree to serialize
** \param   space - pointer to NULL terminated string containing the text to indent each level of the
**                  output with, or NULL if the output should not be indented
**
** \return  pointer to dynamically allocated NULL terminated string containing the JSON text
**          NOTE: Ownership of the returned string passes to the caller
**
**************************************************************************/
char *json_stringify(const JsonNode *node, const char *space)
{
    SB sb;

    SbInit(&sb);

    // Emit the value, indenting it, if the caller requested indentation
    if (space != NULL)
    {
        EmitValue_Indented(&sb, node, space, 0);
    }
    else
    {
        EmitValue(&sb, node);
    }

    return SbFinish(&sb);
}

/*********************************************************************//**
**
** json_delete
**
** Frees the specified node, and all of its descendents, removing it from its parent (if it has one)
**
** \param   node - pointer to node to free, or NULL if there is nothing to free
**
** \return  None
**
**************************************************************************/
void json_delete(JsonNode *node)
{
    JsonNode *child;
    JsonNode *next;

    if (node == NULL)
    {
        return;
    }

    json_remove_from_parent(node);

    switch (node->tag)
    {
        case JSON_STRING:
            free(node->string_);
            break;

        case JSON_ARRAY:
        case JSON_OBJECT:
            child = node->children.head;
            while (child != NULL)
            {
                next = child->next;
                json_delete(child);
                child = next;
            }
            break;

        default:
        case JSON_NULL:
        case JSON_BOOL:
        case JSON_NUMBER:
#if 1
// Copyright (C) 2022, Broadband Forum
// Copyright (C) 2022  CommScope, Inc
        case JSON_LL_NUMBER:
        case JSON_ULL_NUMBER:
#endif
            // Nothing to free for these types
            break;
    }

    free(node);
}

/*********************************************************************//**
**
** json_find_element
**
** Finds the element at the specified index in the specified JSON array
**
** \param   array - pointer to node containing the array to search
** \param   index - index of the element to find (starting at 0)
**
** \return  pointer to the element, or NULL if it was not found
**
**************************************************************************/
JsonNode *json_find_element(JsonNode *array, int index)
{
    JsonNode *element;
    int i;

    if ((array == NULL) || (array->tag != JSON_ARRAY))
    {
        return NULL;
    }

    i = 0;
    json_foreach(element, array)
    {
        if (i == index)
        {
            return element;
        }
        i++;
    }

    return NULL;
}

/*********************************************************************//**
**
** json_find_member
**
** Finds the member with the specified name in the specified JSON object
**
** \param   object - pointer to node containing the object to search
** \param   name - name of the member to find
**
** \return  pointer to the member, or NULL if it was not found
**
**************************************************************************/
JsonNode *json_find_member(JsonNode *object, const char *name)
{
    JsonNode *member;

    if ((object == NULL) || (object->tag != JSON_OBJECT))
    {
        return NULL;
    }

    json_foreach(member, object)
    {
        if (strcmp(member->key, name) == 0)
        {
            return member;
        }
    }

    return NULL;
}

/*********************************************************************//**
**
** json_first_child
**
** Gets the first child of the specified JSON array or object
**
** \param   node - pointer to node containing the array or object
**
** \return  pointer to the first child, or NULL if there are no children, or the node is not an array or object
**
**************************************************************************/
JsonNode *json_first_child(const JsonNode *node)
{
    if (node == NULL)
    {
        return NULL;
    }

    if ((node->tag != JSON_ARRAY) && (node->tag != JSON_OBJECT))
    {
        return NULL;
    }

    return node->children.head;
}

/*********************************************************************//**
**
** json_mknull
**
** Creates a node representing the JSON null value
**
** \param   None
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *json_mknull(void)
{
    return MakeNode(JSON_NULL);
}

/*********************************************************************//**
**
** json_mkbool
**
** Creates a node representing the specified JSON boolean value
**
** \param   b - boolean value that the node represents
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *json_mkbool(bool b)
{
    JsonNode *ret;

    ret = MakeNode(JSON_BOOL);
    ret->bool_ = b;

    return ret;
}

/*********************************************************************//**
**
** json_mkstring
**
** Creates a node representing the specified JSON string value
**
** \param   s - pointer to NULL terminated string that the node represents. This function takes a copy of it
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *json_mkstring(const char *s)
{
    return MakeString(StrDup(s));
}

/*********************************************************************//**
**
** json_mknumber
**
** Creates a node representing the specified JSON number value
**
** \param   n - number that the node represents
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *json_mknumber(double n)
{
    JsonNode *node;

    node = MakeNode(JSON_NUMBER);
    node->number_ = n;

    return node;
}

#if 1
// Copyright (C) 2022, Broadband Forum
// Copyright (C) 2022  CommScope, Inc
/*********************************************************************//**
**
** json_mklonglong
**
** Creates a node representing the specified JSON number value, without any loss of precision for large integers
**
** \param   n - number that the node represents
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *json_mklonglong(long long n)
{
    JsonNode *node;

    node = MakeNode(JSON_LL_NUMBER);
    node->ll_number_ = n;

    return node;
}

/*********************************************************************//**
**
** json_mkulonglong
**
** Creates a node representing the specified JSON number value, without any loss of precision for large integers
**
** \param   n - number that the node represents
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *json_mkulonglong(unsigned long long n)
{
    JsonNode *node;

    node = MakeNode(JSON_ULL_NUMBER);
    node->ull_number_ = n;

    return node;
}
#endif

/*********************************************************************//**
**
** json_mkarray
**
** Creates a node representing an empty JSON array
**
** \param   None
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *json_mkarray(void)
{
    return MakeNode(JSON_ARRAY);
}

/*********************************************************************//**
**
** json_mkobject
**
** Creates a node representing an empty JSON object
**
** \param   None
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *json_mkobject(void)
{
    return MakeNode(JSON_OBJECT);
}

/*********************************************************************//**
**
** json_append_element
**
** Adds the specified element to the end of the specified JSON array
**
** \param   array - pointer to node containing the array to add the element to
** \param   element - pointer to node containing the element to add
**                    NOTE: Ownership of the element passes to the array
**
** \return  None
**
**************************************************************************/
void json_append_element(JsonNode *array, JsonNode *element)
{
    assert(array->tag == JSON_ARRAY);
    assert(element->parent == NULL);

    AppendNode(array, element);
}

/*********************************************************************//**
**
** json_prepend_element
**
** Adds the specified element to the start of the specified JSON array
**
** \param   array - pointer to node containing the array to add the element to
** \param   element - pointer to node containing the element to add
**                    NOTE: Ownership of the element passes to the array
**
** \return  None
**
**************************************************************************/
void json_prepend_element(JsonNode *array, JsonNode *element)
{
    assert(array->tag == JSON_ARRAY);
    assert(element->parent == NULL);

    PrependNode(array, element);
}

/*********************************************************************//**
**
** json_append_member
**
** Adds the specified member to the end of the specified JSON object
**
** \param   object - pointer to node containing the object to add the member to
** \param   key - name of the member to add. This function takes a copy of it
** \param   value - pointer to node containing the value of the member to add
**                  NOTE: Ownership of the value passes to the object
**
** \return  None
**
**************************************************************************/
void json_append_member(JsonNode *object, const char *key, JsonNode *value)
{
    assert(object->tag == JSON_OBJECT);
    assert(value->parent == NULL);

    AppendMember(object, StrDup(key), value);
}

/*********************************************************************//**
**
** json_prepend_member
**
** Adds the specified member to the start of the specified JSON object
**
** \param   object - pointer to node containing the object to add the member to
** \param   key - name of the member to add. This function takes a copy of it
** \param   value - pointer to node containing the value of the member to add
**                  NOTE: Ownership of the value passes to the object
**
** \return  None
**
**************************************************************************/
void json_prepend_member(JsonNode *object, const char *key, JsonNode *value)
{
    assert(object->tag == JSON_OBJECT);
    assert(value->parent == NULL);

    value->key = StrDup(key);
    PrependNode(object, value);
}

/*********************************************************************//**
**
** json_remove_from_parent
**
** Removes the specified node from the array or object containing it, freeing the node's key
**
** \param   node - pointer to node to remove. It is not an error for the node to have no parent
**
** \return  None
**
**************************************************************************/
void json_remove_from_parent(JsonNode *node)
{
    JsonNode *parent;

    parent = node->parent;
    if (parent == NULL)
    {
        return;
    }

    // Unlink the node from the list of children of its parent
    if (node->prev != NULL)
    {
        node->prev->next = node->next;
    }
    else
    {
        parent->children.head = node->next;
    }

    if (node->next != NULL)
    {
        node->next->prev = node->prev;
    }
    else
    {
        parent->children.tail = node->prev;
    }

    free(node->key);

    node->parent = NULL;
    node->prev = NULL;
    node->next = NULL;
    node->key = NULL;
}

/*********************************************************************//**
**
** json_check
**
** Looks for structure and encoding problems in the specified node, and all of its descendents
**
** \param   node - pointer to node to check
** \param   errmsg - pointer to buffer in which to return a description of the first problem found,
**                   or NULL if the caller is not interested in the description
**
** \return  true if no problems were found, false otherwise
**
**************************************************************************/
bool json_check(const JsonNode *node, char errmsg[ERR_MSG_LEN])
{
    bool is_valid;

    if (node->key != NULL)
    {
        is_valid = Utf8ValidateString(node->key);
        if (is_valid == false)
        {
            return ReportProblem(errmsg, "key contains invalid UTF-8");
        }
    }

    is_valid = IsValidTag(node->tag);
    if (is_valid == false)
    {
        return ReportProblem(errmsg, "tag is invalid (%u)", node->tag);
    }

    switch (node->tag)
    {
        case JSON_BOOL:
            if ((node->bool_ != false) && (node->bool_ != true))
            {
                return ReportProblem(errmsg, "bool_ is neither false (%d) nor true (%d)", (int)false, (int)true);
            }
            break;

        case JSON_STRING:
            if (node->string_ == NULL)
            {
                return ReportProblem(errmsg, "string_ is NULL");
            }

            is_valid = Utf8ValidateString(node->string_);
            if (is_valid == false)
            {
                return ReportProblem(errmsg, "string_ contains invalid UTF-8");
            }
            break;

        case JSON_ARRAY:
        case JSON_OBJECT:
            return CheckChildren(node, errmsg);
            break;

        default:
        case JSON_NULL:
        case JSON_NUMBER:
#if 1
// Copyright (C) 2022, Broadband Forum
// Copyright (C) 2022  CommScope, Inc
        case JSON_LL_NUMBER:
        case JSON_ULL_NUMBER:
#endif
            // Nothing more to check for these types
            break;
    }

    return true;
}

/*********************************************************************//**
**
** CheckChildren
**
** Looks for structure and encoding problems in the children of the specified JSON array or object
**
** \param   node - pointer to node containing the array or object to check
** \param   errmsg - pointer to buffer in which to return a description of the first problem found,
**                   or NULL if the caller is not interested in the description
**
** \return  true if no problems were found, false otherwise
**
**************************************************************************/
bool CheckChildren(const JsonNode *node, char *errmsg)
{
    JsonNode *head;
    JsonNode *tail;
    JsonNode *child;
    JsonNode *last;
    bool is_valid;

    head = node->children.head;
    tail = node->children.tail;

    // Exit if the array or object is empty. In this case both head and tail pointers must be NULL
    if ((head == NULL) || (tail == NULL))
    {
        if (head != NULL)
        {
            return ReportProblem(errmsg, "tail is NULL, but head is not");
        }

        if (tail != NULL)
        {
            return ReportProblem(errmsg, "head is NULL, but tail is not");
        }

        return true;
    }

    if (head->prev != NULL)
    {
        return ReportProblem(errmsg, "First child's prev pointer is not NULL");
    }

    // Iterate over all children, checking that they link back to this node correctly
    last = NULL;
    child = head;
    while (child != NULL)
    {
        if (child == node)
        {
            return ReportProblem(errmsg, "node is its own child");
        }

        if (child->next == child)
        {
            return ReportProblem(errmsg, "child->next == child (cycle)");
        }

        if (child->next == head)
        {
            return ReportProblem(errmsg, "child->next == head (cycle)");
        }

        if (child->parent != node)
        {
            return ReportProblem(errmsg, "child does not point back to parent");
        }

        if ((child->next != NULL) && (child->next->prev != child))
        {
            return ReportProblem(errmsg, "child->next does not point back to child");
        }

        // Only members of an object have a key
        if ((node->tag == JSON_ARRAY) && (child->key != NULL))
        {
            return ReportProblem(errmsg, "Array element's key is not NULL");
        }

        if ((node->tag == JSON_OBJECT) && (child->key == NULL))
        {
            return ReportProblem(errmsg, "Object member's key is NULL");
        }

        is_valid = json_check(child, errmsg);
        if (is_valid == false)
        {
            return false;
        }

        last = child;
        child = child->next;
    }

    if (last != tail)
    {
        return ReportProblem(errmsg, "tail does not match pointer found by starting at head and following next links");
    }

    return true;
}

/*********************************************************************//**
**
** ReportProblem
**
** Writes a description of a problem found by json_check() into the caller's buffer
**
** \param   errmsg - pointer to buffer in which to return the description of the problem,
**                   or NULL if the caller is not interested in the description
** \param   fmt - printf style format string describing the problem
**
** \return  false always, so that callers can use this function in their return statement
**
**************************************************************************/
bool ReportProblem(char *errmsg, const char *fmt, ...)
{
    va_list ap;

    if (errmsg == NULL)
    {
        return false;
    }

    va_start(ap, fmt);
    vsnprintf(errmsg, ERR_MSG_LEN, fmt, ap);
    va_end(ap);

    return false;
}

/*********************************************************************//**
**
** ParseValue
**
** Parses a JSON value from the specified position in the JSON text
**
** \param   sp - pointer to variable containing the position in the JSON text to parse from.
**               On successful exit, this is updated to point to the first character after the value
** \param   out - pointer to variable in which to return the node representing the value,
**                or NULL if the caller only wants to validate the text
**
** \return  true if a value was parsed successfully, false otherwise
**
**************************************************************************/
bool ParseValue(const char **sp, JsonNode **out)
{
    const char *s;
    char *str;
    char **str_arg;
    double num;
    double *num_arg;
    bool result;

    s = *sp;
    switch (s[0])
    {
        case 'n':
            result = ExpectLiteral(&s, "null");
            if (result == false)
            {
                return false;
            }

            if (out != NULL)
            {
                *out = json_mknull();
            }
            break;

        case 'f':
            result = ExpectLiteral(&s, "false");
            if (result == false)
            {
                return false;
            }

            if (out != NULL)
            {
                *out = json_mkbool(false);
            }
            break;

        case 't':
            result = ExpectLiteral(&s, "true");
            if (result == false)
            {
                return false;
            }

            if (out != NULL)
            {
                *out = json_mkbool(true);
            }
            break;

        case '"':
            str = NULL;
            str_arg = NULL;
            if (out != NULL)
            {
                str_arg = &str;
            }

            result = ParseString(&s, str_arg);
            if (result == false)
            {
                return false;
            }

            if (out != NULL)
            {
                *out = MakeString(str);
            }
            break;

        case '[':
            result = ParseArray(&s, out);
            if (result == false)
            {
                return false;
            }
            break;

        case '{':
            result = ParseObject(&s, out);
            if (result == false)
            {
                return false;
            }
            break;

        default:
            num_arg = NULL;
            if (out != NULL)
            {
                num_arg = &num;
            }

            result = ParseNumber(&s, num_arg);
            if (result == false)
            {
                return false;
            }

            if (out != NULL)
            {
                *out = json_mknumber(num);
            }
            break;
    }

    *sp = s;
    return true;
}

/*********************************************************************//**
**
** ParseArray
**
** Parses a JSON array from the specified position in the JSON text
**
** \param   sp - pointer to variable containing the position in the JSON text to parse from.
**               On successful exit, this is updated to point to the first character after the array
** \param   out - pointer to variable in which to return the node representing the array,
**                or NULL if the caller only wants to validate the text
**
** \return  true if an array was parsed successfully, false otherwise
**
**************************************************************************/
bool ParseArray(const char **sp, JsonNode **out)
{
    const char *s;
    JsonNode *ret;
    JsonNode *element;
    JsonNode **element_arg;
    bool result;

    s = *sp;
    ret = NULL;
    element_arg = NULL;
    if (out != NULL)
    {
        ret = json_mkarray();
        element_arg = &element;
    }

    if (s[0] != '[')
    {
        goto failure;
    }
    s++;
    SkipSpace(&s);

    // Exit if the array is empty
    if (s[0] == ']')
    {
        s++;
        goto success;
    }

    // Parse each element of the array in turn
    while (true)
    {
        result = ParseValue(&s, element_arg);
        if (result == false)
        {
            goto failure;
        }
        SkipSpace(&s);

        if (out != NULL)
        {
            json_append_element(ret, element);
        }

        // Exit the loop if this was the last element in the array
        if (s[0] == ']')
        {
            s++;
            goto success;
        }

        // Otherwise the next element must be preceded by a comma
        if (s[0] != ',')
        {
            goto failure;
        }
        s++;
        SkipSpace(&s);
    }

success:
    *sp = s;
    if (out != NULL)
    {
        *out = ret;
    }
    return true;

failure:
    json_delete(ret);
    return false;
}

/*********************************************************************//**
**
** ParseObject
**
** Parses a JSON object from the specified position in the JSON text
**
** \param   sp - pointer to variable containing the position in the JSON text to parse from.
**               On successful exit, this is updated to point to the first character after the object
** \param   out - pointer to variable in which to return the node representing the object,
**                or NULL if the caller only wants to validate the text
**
** \return  true if an object was parsed successfully, false otherwise
**
**************************************************************************/
bool ParseObject(const char **sp, JsonNode **out)
{
    const char *s;
    JsonNode *ret;
    JsonNode *value;
    JsonNode **value_arg;
    char *key;
    char **key_arg;
    bool result;

    s = *sp;
    ret = NULL;
    key = NULL;
    key_arg = NULL;
    value_arg = NULL;
    if (out != NULL)
    {
        ret = json_mkobject();
        key_arg = &key;
        value_arg = &value;
    }

    if (s[0] != '{')
    {
        goto failure;
    }
    s++;
    SkipSpace(&s);

    // Exit if the object is empty
    if (s[0] == '}')
    {
        s++;
        goto success;
    }

    // Parse each member of the object in turn
    while (true)
    {
        result = ParseString(&s, key_arg);
        if (result == false)
        {
            goto failure;
        }
        SkipSpace(&s);

        if (s[0] != ':')
        {
            goto failure_free_key;
        }
        s++;
        SkipSpace(&s);

        result = ParseValue(&s, value_arg);
        if (result == false)
        {
            goto failure_free_key;
        }
        SkipSpace(&s);

        if (out != NULL)
        {
            AppendMember(ret, key, value);
        }

        // Exit the loop if this was the last member of the object
        if (s[0] == '}')
        {
            s++;
            goto success;
        }

        // Otherwise the next member must be preceded by a comma
        if (s[0] != ',')
        {
            goto failure;
        }
        s++;
        SkipSpace(&s);
    }

success:
    *sp = s;
    if (out != NULL)
    {
        *out = ret;
    }
    return true;

failure_free_key:
    // NOTE: The key has not been added to the object yet, so it has to be freed separately
    if (out != NULL)
    {
        free(key);
    }

failure:
    json_delete(ret);
    return false;
}

/*********************************************************************//**
**
** ParseString
**
** Parses a JSON string from the specified position in the JSON text
**
** \param   sp - pointer to variable containing the position in the JSON text to parse from.
**               On successful exit, this is updated to point to the first character after the string
** \param   out - pointer to variable in which to return the unescaped string,
**                or NULL if the caller only wants to validate the text
**                NOTE: Ownership of the returned string passes to the caller
**
** \return  true if a string was parsed successfully, false otherwise
**
**************************************************************************/
bool ParseString(const char **sp, char **out)
{
    const char *s;
    SB sb;
    char throwaway_buffer[MAX_UTF8_CHAR_LEN];
    char *b;
    int count;

    // NOTE: sb is zeroed to prevent the compiler complaining about possible use of an uninitialised
    // variable in the call to SbFree() (Copyright 2018 CommScope, Inc)
    memset(&sb, 0, sizeof(sb));

    s = *sp;
    if (s[0] != '"')
    {
        return false;
    }
    s++;

    // Set up the buffer that each parsed character is written to.
    // If the caller doesn't want the string, then the characters are just thrown away
    if (out != NULL)
    {
        SbInit(&sb);
        SbNeed(&sb, MAX_UTF8_CHAR_LEN);
        b = sb.cur;
    }
    else
    {
        b = throwaway_buffer;
    }

    while (s[0] != '"')
    {
        // Parse the next character, writing it to b
        count = ParseString_Char(&s, b);
        if (count == -1)
        {
            goto failed;
        }

        // Update sb to know about the new bytes, and set up b to write another character
        if (out != NULL)
        {
            sb.cur = &b[count];
            SbNeed(&sb, MAX_UTF8_CHAR_LEN);
            b = sb.cur;
        }
        else
        {
            b = throwaway_buffer;
        }
    }
    s++;

    if (out != NULL)
    {
        *out = SbFinish(&sb);
    }
    *sp = s;
    return true;

failed:
    if (out != NULL)
    {
        SbFree(&sb);
    }
    return false;
}

/*********************************************************************//**
**
** ParseString_Char
**
** Parses the next character of a JSON string
**
** \param   sp - pointer to variable containing the position in the JSON text of the character to parse.
**               On successful exit, this is updated to point to the first character after the parsed character
** \param   b - pointer to buffer in which to return the unescaped character
**              NOTE: This buffer must have space for MAX_UTF8_CHAR_LEN bytes
**
** \return  number of bytes written to the buffer, or -1 if the character was invalid
**
**************************************************************************/
int ParseString_Char(const char **sp, char *b)
{
    const char *s;
    int len;

    s = *sp;

    // Handle an escape sequence
    if (s[0] == '\\')
    {
        s++;
        *sp = s;
        return ParseString_Escape(sp, b);
    }

    // Exit if the character is a control character, as these are not allowed in string literals
    if ((unsigned char)s[0] <= 0x1F)
    {
        return -1;
    }

    // Otherwise validate and echo a UTF-8 character
    len = Utf8ValidateChar(s);
    if (len == 0)
    {
        return -1;
    }

    memcpy(b, s, len);
    *sp = &s[len];

    return len;
}

/*********************************************************************//**
**
** ParseString_Escape
**
** Parses the character following the backslash of an escape sequence in a JSON string
**
** \param   sp - pointer to variable containing the position in the JSON text of the character after the backslash.
**               On successful exit, this is updated to point to the first character after the escape sequence
** \param   b - pointer to buffer in which to return the unescaped character
**              NOTE: This buffer must have space for MAX_UTF8_CHAR_LEN bytes
**
** \return  number of bytes written to the buffer, or -1 if the escape sequence was invalid
**
**************************************************************************/
int ParseString_Escape(const char **sp, char *b)
{
    const char *s;
    char c;

    s = *sp;
    c = s[0];
    s++;

    switch (c)
    {
        case '"':
        case '\\':
        case '/':
            b[0] = c;
            break;

        case 'b':
            b[0] = '\b';
            break;

        case 'f':
            b[0] = '\f';
            break;

        case 'n':
            b[0] = '\n';
            break;

        case 'r':
            b[0] = '\r';
            break;

        case 't':
            b[0] = '\t';
            break;

        case 'u':
            return ParseString_Unicode(sp, b);
            break;

        default:
            // Invalid escape
            return -1;
            break;
    }

    *sp = s;
    return 1;
}

/*********************************************************************//**
**
** ParseString_Unicode
**
** Parses the codepoint following the '\u' of a unicode escape sequence in a JSON string,
** writing it to the caller's buffer in UTF-8 form
**
** \param   sp - pointer to variable containing the position in the JSON text of the character after the 'u'.
**               On successful exit, this is updated to point to the first character after the escape sequence
** \param   b - pointer to buffer in which to return the UTF-8 encoded character
**              NOTE: This buffer must have space for MAX_UTF8_CHAR_LEN bytes
**
** \return  number of bytes written to the buffer, or -1 if the escape sequence was invalid
**
**************************************************************************/
int ParseString_Unicode(const char **sp, char *b)
{
    const char *s;
    uint16_t uc;
    uint16_t lc;
    uchar_t unicode;
    int count;
    bool result;

    s = *sp;
    s++;                    // Skip the 'u' of the escape sequence

    result = ParseHex16(&s, &uc);
    if (result == false)
    {
        return -1;
    }

    // Exit if the escape sequence is "\u0000", as this is not allowed
    if (uc == 0)
    {
        return -1;
    }

    unicode = uc;
    if ((uc >= 0xD800) && (uc <= 0xDFFF))
    {
        // Handle UTF-16 surrogate pair. The second half of the pair must be escaped using '\u' too
        if (s[0] != '\\')
        {
            return -1;      // Incomplete surrogate pair
        }
        s++;

        if (s[0] != 'u')
        {
            return -1;      // Incomplete surrogate pair
        }
        s++;

        result = ParseHex16(&s, &lc);
        if (result == false)
        {
            return -1;      // Incomplete surrogate pair
        }

        result = FromSurrogatePair(uc, lc, &unicode);
        if (result == false)
        {
            return -1;      // Invalid surrogate pair
        }
    }

    count = Utf8WriteChar(unicode, b);
    *sp = s;

    return count;
}

/*********************************************************************//**
**
** ParseNumber
**
** Parses a JSON number from the specified position in the JSON text
**
** NOTE: The JSON spec says that a number shall follow this precise pattern
** (spaces and quotes added for readability):
**    '-'? (0 | [1-9][0-9]*) ('.' [0-9]+)? ([Ee] [+-]? [0-9]+)?
** However, some JSON parsers are more liberal. For instance, PHP accepts '.5' and '1.'.
** JSON.parse accepts '+3'. This function takes the strict approach
**
** \param   sp - pointer to variable containing the position in the JSON text to parse from.
**               On successful exit, this is updated to point to the first character after the number
** \param   out - pointer to variable in which to return the parsed number,
**                or NULL if the caller only wants to validate the text
**
** \return  true if a number was parsed successfully, false otherwise
**
**************************************************************************/
bool ParseNumber(const char **sp, double *out)
{
    const char *s;

    s = *sp;

    // '-'?
    if (s[0] == '-')
    {
        s++;
    }

    // (0 | [1-9][0-9]*)
    if (s[0] == '0')
    {
        s++;
    }
    else
    {
        if (IS_DIGIT(s[0]) == false)
        {
            return false;
        }

        do
        {
            s++;
        } while (IS_DIGIT(s[0]));
    }

    // ('.' [0-9]+)?
    if (s[0] == '.')
    {
        s++;
        if (IS_DIGIT(s[0]) == false)
        {
            return false;
        }

        do
        {
            s++;
        } while (IS_DIGIT(s[0]));
    }

    // ([Ee] [+-]? [0-9]+)?
    if ((s[0] == 'E') || (s[0] == 'e'))
    {
        s++;
        if ((s[0] == '+') || (s[0] == '-'))
        {
            s++;
        }

        if (IS_DIGIT(s[0]) == false)
        {
            return false;
        }

        do
        {
            s++;
        } while (IS_DIGIT(s[0]));
    }

    if (out != NULL)
    {
        *out = strtod(*sp, NULL);
    }

    *sp = s;
    return true;
}

/*********************************************************************//**
**
** ParseHex16
**
** Parses exactly 4 hex characters (capital or lowercase) from the specified position in the JSON text
**
** \param   sp - pointer to variable containing the position in the JSON text to parse from.
**               On successful exit, this is updated to point to the first character after the hex characters
** \param   out - pointer to variable in which to return the parsed value,
**                or NULL if the caller only wants to validate the text
**
** \return  true if 4 hex characters were parsed successfully, false if any of the characters were not [0-9A-Fa-f]
**
**************************************************************************/
bool ParseHex16(const char **sp, uint16_t *out)
{
    const char *s;
    uint16_t ret;
    int tmp;
    char c;
    int i;

    s = *sp;
    ret = 0;
    for (i=0; i < HEX16_LEN; i++)
    {
        c = s[0];
        s++;

        tmp = HexToNibble(c);
        if (tmp == -1)
        {
            return false;
        }

        ret <<= 4;
        ret += tmp;
    }

    if (out != NULL)
    {
        *out = ret;
    }
    *sp = s;

    return true;
}

/*********************************************************************//**
**
** HexToNibble
**
** Converts a single hexadecimal character (capital or lowercase) into its value
**
** \param   c - character to convert
**
** \return  value of the character, or -1 if the character was not [0-9A-Fa-f]
**
**************************************************************************/
int HexToNibble(char c)
{
    if ((c >= '0') && (c <= '9'))
    {
        return c - '0';
    }

    if ((c >= 'A') && (c <= 'F'))
    {
        return c - 'A' + 10;
    }

    if ((c >= 'a') && (c <= 'f'))
    {
        return c - 'a' + 10;
    }

    return -1;
}

/*********************************************************************//**
**
** ExpectLiteral
**
** Determines whether the JSON text at the specified position starts with the specified literal
**
** \param   sp - pointer to variable containing the position in the JSON text to parse from.
**               On successful exit, this is updated to point to the first character after the literal
** \param   str - pointer to NULL terminated string containing the literal to match
**
** \return  true if the JSON text starts with the literal, false otherwise
**
**************************************************************************/
bool ExpectLiteral(const char **sp, const char *str)
{
    const char *s;
    int i;

    s = *sp;
    i = 0;
    while (str[i] != '\0')
    {
        if (s[i] != str[i])
        {
            return false;
        }
        i++;
    }

    *sp = &s[i];
    return true;
}

/*********************************************************************//**
**
** SkipSpace
**
** Moves the specified position in the JSON text past any whitespace characters
**
** \param   sp - pointer to variable containing the position in the JSON text.
**               On exit, this is updated to point to the first character which is not whitespace
**
** \return  None
**
**************************************************************************/
void SkipSpace(const char **sp)
{
    const char *s;

    s = *sp;
    while (IS_SPACE(s[0]))
    {
        s++;
    }

    *sp = s;
}

/*********************************************************************//**
**
** EmitValue
**
** Serializes the specified node (and all of its descendents) into the specified string buffer
**
** \param   out - pointer to string buffer to write the serialized node into
** \param   node - pointer to node to serialize
**
** \return  None
**
**************************************************************************/
void EmitValue(SB *out, const JsonNode *node)
{
    assert(IsValidTag(node->tag) == true);

    switch (node->tag)
    {
        case JSON_NULL:
            SbPuts(out, "null");
            break;

        case JSON_BOOL:
            if (node->bool_ == true)
            {
                SbPuts(out, "true");
            }
            else
            {
                SbPuts(out, "false");
            }
            break;

        case JSON_STRING:
            EmitString(out, node->string_);
            break;

        case JSON_NUMBER:
            EmitNumber(out, node->number_);
            break;

#if 1
// Copyright (C) 2022, Broadband Forum
// Copyright (C) 2022  CommScope, Inc
        case JSON_LL_NUMBER:
            EmitNumber_LongLong(out, node->ll_number_);
            break;

        case JSON_ULL_NUMBER:
            EmitNumber_UnsignedLongLong(out, node->ull_number_);
            break;
#endif

        case JSON_ARRAY:
            EmitArray(out, node);
            break;

        case JSON_OBJECT:
            EmitObject(out, node);
            break;

        default:
            assert(false);
            break;
    }
}

/*********************************************************************//**
**
** EmitValue_Indented
**
** Serializes the specified node (and all of its descendents) into the specified string buffer,
** indenting each level of the output
**
** \param   out - pointer to string buffer to write the serialized node into
** \param   node - pointer to node to serialize
** \param   space - pointer to NULL terminated string containing the text to indent each level of the output with
** \param   indent_level - number of levels that this node is indented by
**
** \return  None
**
**************************************************************************/
void EmitValue_Indented(SB *out, const JsonNode *node, const char *space, int indent_level)
{
    assert(IsValidTag(node->tag) == true);

    switch (node->tag)
    {
        case JSON_NULL:
            SbPuts(out, "null");
            break;

        case JSON_BOOL:
            if (node->bool_ == true)
            {
                SbPuts(out, "true");
            }
            else
            {
                SbPuts(out, "false");
            }
            break;

        case JSON_STRING:
            EmitString(out, node->string_);
            break;

        case JSON_NUMBER:
            EmitNumber(out, node->number_);
            break;

#if 1
// Copyright (C) 2022, Broadband Forum
// Copyright (C) 2022  CommScope, Inc
        case JSON_LL_NUMBER:
            EmitNumber_LongLong(out, node->ll_number_);
            break;

        case JSON_ULL_NUMBER:
            EmitNumber_UnsignedLongLong(out, node->ull_number_);
            break;
#endif

        case JSON_ARRAY:
            EmitArray_Indented(out, node, space, indent_level);
            break;

        case JSON_OBJECT:
            EmitObject_Indented(out, node, space, indent_level);
            break;

        default:
            assert(false);
            break;
    }
}

/*********************************************************************//**
**
** EmitArray
**
** Serializes the specified JSON array into the specified string buffer
**
** \param   out - pointer to string buffer to write the serialized array into
** \param   array - pointer to node containing the array to serialize
**
** \return  None
**
**************************************************************************/
void EmitArray(SB *out, const JsonNode *array)
{
    const JsonNode *element;

    SbPutChar(out, '[');
    json_foreach(element, array)
    {
        EmitValue(out, element);

        // Elements are separated by a comma
        if (element->next != NULL)
        {
            SbPutChar(out, ',');
        }
    }
    SbPutChar(out, ']');
}

/*********************************************************************//**
**
** EmitArray_Indented
**
** Serializes the specified JSON array into the specified string buffer, indenting each of its elements
**
** \param   out - pointer to string buffer to write the serialized array into
** \param   array - pointer to node containing the array to serialize
** \param   space - pointer to NULL terminated string containing the text to indent each level of the output with
** \param   indent_level - number of levels that the array is indented by
**
** \return  None
**
**************************************************************************/
void EmitArray_Indented(SB *out, const JsonNode *array, const char *space, int indent_level)
{
    const JsonNode *element;

    // Exit if the array is empty
    element = array->children.head;
    if (element == NULL)
    {
        SbPuts(out, "[]");
        return;
    }

    SbPuts(out, "[\n");
    while (element != NULL)
    {
        EmitIndent(out, space, indent_level + 1);
        EmitValue_Indented(out, element, space, indent_level + 1);

        // Elements are separated by a comma
        element = element->next;
        if (element != NULL)
        {
            SbPuts(out, ",\n");
        }
        else
        {
            SbPuts(out, "\n");
        }
    }

    EmitIndent(out, space, indent_level);
    SbPutChar(out, ']');
}

/*********************************************************************//**
**
** EmitObject
**
** Serializes the specified JSON object into the specified string buffer
**
** \param   out - pointer to string buffer to write the serialized object into
** \param   object - pointer to node containing the object to serialize
**
** \return  None
**
**************************************************************************/
void EmitObject(SB *out, const JsonNode *object)
{
    const JsonNode *member;

    SbPutChar(out, '{');
    json_foreach(member, object)
    {
        EmitString(out, member->key);
        SbPutChar(out, ':');
        EmitValue(out, member);

        // Members are separated by a comma
        if (member->next != NULL)
        {
            SbPutChar(out, ',');
        }
    }
    SbPutChar(out, '}');
}

/*********************************************************************//**
**
** EmitObject_Indented
**
** Serializes the specified JSON object into the specified string buffer, indenting each of its members
**
** \param   out - pointer to string buffer to write the serialized object into
** \param   object - pointer to node containing the object to serialize
** \param   space - pointer to NULL terminated string containing the text to indent each level of the output with
** \param   indent_level - number of levels that the object is indented by
**
** \return  None
**
**************************************************************************/
void EmitObject_Indented(SB *out, const JsonNode *object, const char *space, int indent_level)
{
    const JsonNode *member;

    // Exit if the object is empty
    member = object->children.head;
    if (member == NULL)
    {
        SbPuts(out, "{}");
        return;
    }

    SbPuts(out, "{\n");
    while (member != NULL)
    {
        EmitIndent(out, space, indent_level + 1);
        EmitString(out, member->key);
        SbPuts(out, ": ");
        EmitValue_Indented(out, member, space, indent_level + 1);

        // Members are separated by a comma
        member = member->next;
        if (member != NULL)
        {
            SbPuts(out, ",\n");
        }
        else
        {
            SbPuts(out, "\n");
        }
    }

    EmitIndent(out, space, indent_level);
    SbPutChar(out, '}');
}

/*********************************************************************//**
**
** EmitIndent
**
** Writes the indentation for the specified level into the specified string buffer
**
** \param   out - pointer to string buffer to write the indentation into
** \param   space - pointer to NULL terminated string containing the text to indent each level of the output with
** \param   indent_level - number of levels to indent by
**
** \return  None
**
**************************************************************************/
void EmitIndent(SB *out, const char *space, int indent_level)
{
    int i;

    for (i=0; i < indent_level; i++)
    {
        SbPuts(out, space);
    }
}

/*********************************************************************//**
**
** EmitString
**
** Serializes the specified string into the specified string buffer, quoting and escaping it
**
** \param   out - pointer to string buffer to write the serialized string into
** \param   str - pointer to NULL terminated string to serialize
**
** \return  None
**
**************************************************************************/
void EmitString(SB *out, const char *str)
{
    bool escape_unicode = false;
    const char *s;
    char *b;
    unsigned char c;
    int count;

#if 0
    // Copyright (C) 2023, Broadband Forum
    // Copyright (C) 2023  CommScope, Inc
    // Allow non UTF-8 characters to be represented by the Unicode replacement character (U+FFFD)
    // assert(Utf8ValidateString(str) == true);
#endif

    s = str;
    SbNeed(out, MAX_EMITTED_CHAR_LEN);
    b = out->cur;

    b[0] = '"';
    b = &b[1];
    while (s[0] != '\0')
    {
        // Encode the next character, and write it to b
        c = (unsigned char) s[0];
        count = EmitString_Escape(b, c);
        if (count > 0)
        {
            s = &s[1];
        }
        else
        {
            count = EmitString_Char(b, &s, escape_unicode);
        }
        b = &b[count];

        // Update out to know about the new bytes, and set up b to write another encoded character
        out->cur = b;
        SbNeed(out, MAX_EMITTED_CHAR_LEN);
        b = out->cur;
    }

    b[0] = '"';
    b = &b[1];

    out->cur = b;
}

/*********************************************************************//**
**
** EmitString_Escape
**
** Writes the two character escape sequence for the specified character into the caller's buffer
**
** \param   b - pointer to buffer in which to return the escape sequence
** \param   c - character to escape
**
** \return  number of bytes written to the buffer, or 0 if the character does not have a two character escape sequence
**
**************************************************************************/
int EmitString_Escape(char *b, unsigned char c)
{
    b[0] = '\\';

    switch (c)
    {
        case '"':
            b[1] = '"';
            break;

        case '\\':
            b[1] = '\\';
            break;

        case '\b':
            b[1] = 'b';
            break;

        case '\f':
            b[1] = 'f';
            break;

        case '\n':
            b[1] = 'n';
            break;

        case '\r':
            b[1] = 'r';
            break;

        case '\t':
            b[1] = 't';
            break;

        default:
            return 0;
            break;
    }

    return 2;
}

/*********************************************************************//**
**
** EmitString_Char
**
** Writes the next character of a string into the caller's buffer, escaping it using '\uXXXX' notation if necessary
**
** \param   b - pointer to buffer in which to return the encoded character
**              NOTE: This buffer must have space for MAX_EMITTED_CHAR_LEN bytes
** \param   sp - pointer to variable containing the position in the string to encode from.
**               On exit, this is updated to point to the first character after the encoded character
** \param   escape_unicode - set if all non-ASCII characters should be escaped using '\uXXXX' notation
**
** \return  number of bytes written to the buffer
**
**************************************************************************/
int EmitString_Char(char *b, const char **sp, bool escape_unicode)
{
    const char *s;
    unsigned char c;
    uchar_t unicode;
    uint16_t uc;
    uint16_t lc;
    int len;
    int count;

    s = *sp;
    c = (unsigned char) s[0];
    len = Utf8ValidateChar(s);

    // Handle an invalid UTF-8 character gracefully in production by writing a replacement
    // character (U+FFFD) and skipping a single byte
    if (len == 0)
    {
#if 0
    // Copyright (C) 2023, Broadband Forum
    // Copyright (C) 2023  CommScope, Inc
    // Allow non UTF-8 characters to be represented by the Unicode replacement character (U+FFFD)
        // assert(false);
#endif
        *sp = &s[1];

        if (escape_unicode == true)
        {
            memcpy(b, "\\uFFFD", 6);
            return 6;
        }

        b[0] = (char) 0xEF;
        b[1] = (char) 0xBF;
        b[2] = (char) 0xBD;
        return 3;
    }

    // Write the character directly, if it does not have to be escaped
    if ((c >= 0x1F) && ((c < 0x80) || (escape_unicode == false)))
    {
        memcpy(b, s, len);
        *sp = &s[len];
        return len;
    }

    // If the code gets here, the character has to be escaped using '\uXXXX' notation
    len = Utf8ReadChar(s, &unicode);
    *sp = &s[len];

    // Handle the case of a character which fits into a single UTF-16 code unit
    if (unicode <= 0xFFFF)
    {
        b[0] = '\\';
        b[1] = 'u';
        count = WriteHex16(&b[2], (uint16_t)unicode);
        return count + 2;
    }

    // Otherwise produce a surrogate pair
    assert(unicode <= 0x10FFFF);
    ToSurrogatePair(unicode, &uc, &lc);
    b[0] = '\\';
    b[1] = 'u';
    count = WriteHex16(&b[2], uc);
    b[count + 2] = '\\';
    b[count + 3] = 'u';
    count += WriteHex16(&b[count + 4], lc);

    return count + 4;
}

/*********************************************************************//**
**
** EmitNumber
**
** Serializes the specified number into the specified string buffer
**
** NOTE: This isn't exactly how JavaScript renders numbers, but it should produce valid JSON for
** reasonable numbers, preserve precision well enough, and avoid some oddities
** like 0.3 -> 0.299999999999999988898
**
** \param   out - pointer to string buffer to write the serialized number into
** \param   num - number to serialize
**
** \return  None
**
**************************************************************************/
void EmitNumber(SB *out, double num)
{
    char buf[64];
    bool is_valid;

    snprintf(buf, sizeof(buf), "%.16g", num);
    buf[sizeof(buf)-1] = '\0';

    // Numbers which cannot be represented in JSON (eg NaN and infinity) are emitted as null
    is_valid = IsValidNumber(buf);
    if (is_valid == false)
    {
        SbPuts(out, "null");
        return;
    }

    SbPuts(out, buf);
}

#if 1
// Copyright (C) 2022, Broadband Forum
// Copyright (C) 2022  CommScope, Inc
/*********************************************************************//**
**
** EmitNumber_LongLong
**
** Serializes the specified number into the specified string buffer, without any loss of precision
**
** \param   out - pointer to string buffer to write the serialized number into
** \param   num - number to serialize
**
** \return  None
**
**************************************************************************/
void EmitNumber_LongLong(SB *out, long long num)
{
    char buf[64];
    bool is_valid;

    snprintf(buf, sizeof(buf), "%lld", num);
    buf[sizeof(buf)-1] = '\0';

    is_valid = IsValidNumber(buf);
    if (is_valid == false)
    {
        SbPuts(out, "null");
        return;
    }

    SbPuts(out, buf);
}

/*********************************************************************//**
**
** EmitNumber_UnsignedLongLong
**
** Serializes the specified number into the specified string buffer, without any loss of precision
**
** \param   out - pointer to string buffer to write the serialized number into
** \param   num - number to serialize
**
** \return  None
**
**************************************************************************/
void EmitNumber_UnsignedLongLong(SB *out, unsigned long long num)
{
    char buf[64];
    bool is_valid;

    snprintf(buf, sizeof(buf), "%llu", num);
    buf[sizeof(buf)-1] = '\0';

    is_valid = IsValidNumber(buf);
    if (is_valid == false)
    {
        SbPuts(out, "null");
        return;
    }

    SbPuts(out, buf);
}
#endif

/*********************************************************************//**
**
** WriteHex16
**
** Encodes the specified 16 bit number into hexadecimal, writing exactly HEX16_LEN hex characters
**
** \param   out - pointer to buffer in which to return the hex characters
** \param   val - number to encode
**
** \return  number of characters written to the buffer
**
**************************************************************************/
int WriteHex16(char *out, uint16_t val)
{
    const char *hex = "0123456789ABCDEF";

    out[0] = hex[(val >> 12) & 0xF];
    out[1] = hex[(val >> 8)  & 0xF];
    out[2] = hex[(val >> 4)  & 0xF];
    out[3] = hex[ val        & 0xF];

    return HEX16_LEN;
}

/*********************************************************************//**
**
** MakeNode
**
** Creates a zeroed node of the specified type
**
** \param   tag - type of the node to create
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *MakeNode(JsonTag tag)
{
    JsonNode *ret;

    ret = (JsonNode *) calloc(1, sizeof(JsonNode));
    if (ret == NULL)
    {
        OutOfMemory();
    }
    ret->tag = tag;

    return ret;
}

/*********************************************************************//**
**
** MakeString
**
** Creates a node representing the specified JSON string value
**
** \param   s - pointer to dynamically allocated NULL terminated string that the node represents
**              NOTE: Ownership of the string passes to the node
**
** \return  pointer to the created node
**          NOTE: Ownership of the returned node passes to the caller
**
**************************************************************************/
JsonNode *MakeString(char *s)
{
    JsonNode *ret;

    ret = MakeNode(JSON_STRING);
    ret->string_ = s;

    return ret;
}

/*********************************************************************//**
**
** AppendNode
**
** Adds the specified child to the end of the list of children of the specified node
**
** \param   parent - pointer to node containing the array or object to add the child to
** \param   child - pointer to node to add
**
** \return  None
**
**************************************************************************/
void AppendNode(JsonNode *parent, JsonNode *child)
{
    child->parent = parent;
    child->prev = parent->children.tail;
    child->next = NULL;

    if (parent->children.tail != NULL)
    {
        parent->children.tail->next = child;
    }
    else
    {
        parent->children.head = child;
    }
    parent->children.tail = child;
}

/*********************************************************************//**
**
** PrependNode
**
** Adds the specified child to the start of the list of children of the specified node
**
** \param   parent - pointer to node containing the array or object to add the child to
** \param   child - pointer to node to add
**
** \return  None
**
**************************************************************************/
void PrependNode(JsonNode *parent, JsonNode *child)
{
    child->parent = parent;
    child->prev = NULL;
    child->next = parent->children.head;

    if (parent->children.head != NULL)
    {
        parent->children.head->prev = child;
    }
    else
    {
        parent->children.tail = child;
    }
    parent->children.head = child;
}

/*********************************************************************//**
**
** AppendMember
**
** Adds the specified member to the end of the list of members of the specified JSON object
**
** \param   object - pointer to node containing the object to add the member to
** \param   key - pointer to dynamically allocated NULL terminated string containing the name of the member
**                NOTE: Ownership of the key passes to the member
** \param   value - pointer to node containing the value of the member to add
**
** \return  None
**
**************************************************************************/
void AppendMember(JsonNode *object, char *key, JsonNode *value)
{
    value->key = key;
    AppendNode(object, value);
}

/*********************************************************************//**
**
** IsValidTag
**
** Determines whether the specified node type is one of the types supported by this module
**
** \param   tag - type of node to check
**
** \return  true if the type is valid, false otherwise
**
**************************************************************************/
bool IsValidTag(unsigned int tag)
{
    if (tag <= JSON_OBJECT)
    {
        return true;
    }

    return false;
}

/*********************************************************************//**
**
** IsValidNumber
**
** Determines whether the specified string can be represented as a JSON number
**
** \param   num - pointer to NULL terminated string containing the number to check
**
** \return  true if the string is a valid JSON number, false otherwise
**
**************************************************************************/
bool IsValidNumber(const char *num)
{
    bool result;

    result = ParseNumber(&num, NULL);
    if ((result == true) && (num[0] == '\0'))
    {
        return true;
    }

    return false;
}

/*********************************************************************//**
**
** Utf8ValidateString
**
** Determines whether the specified NULL terminated string contains only valid UTF-8 characters
**
** \param   s - pointer to NULL terminated string to validate
**
** \return  true if the string is valid UTF-8, false otherwise
**
**************************************************************************/
bool Utf8ValidateString(const char *s)
{
    int len;

    while (s[0] != '\0')
    {
        len = Utf8ValidateChar(s);
        if (len == 0)
        {
            return false;
        }
        s = &s[len];
    }

    return true;
}

/*********************************************************************//**
**
** Utf8ValidateChar
**
** Validates the single UTF-8 character starting at the specified position in a NULL terminated string
**
** NOTE: This function implements the syntax given in RFC3629, which is the same as that given in
** The Unicode Standard, Version 6.0. It has the following properties:
**  - All codepoints U+0000..U+10FFFF may be encoded, except for U+D800..U+DFFF, which are reserved
**    for UTF-16 surrogate pair encoding
**  - UTF-8 byte sequences longer than 4 bytes are not permitted, as they exceed the range of Unicode
**  - The sixty-six Unicode "non-characters" are permitted (namely, U+FDD0..U+FDEF, U+xxFFFE, and U+xxFFFF)
**
** \param   s - pointer to the first byte of the character to validate
**
** \return  length of the character in bytes (1 thru 4), or 0 if the character is invalid or clipped
**
**************************************************************************/
int Utf8ValidateChar(const char *s)
{
    unsigned char c;

    c = (unsigned char) s[0];

    // 00..7F
    if (c <= 0x7F)
    {
        return 1;
    }

    // 80..C1: Disallow overlong 2-byte sequence
    if (c <= 0xC1)
    {
        return 0;
    }

    // C2..DF: Make sure the subsequent byte is in the range 0x80..0xBF
    if (c <= 0xDF)
    {
        if (((unsigned char)s[1] & 0xC0) != 0x80)
        {
            return 0;
        }

        return 2;
    }

    // E0..EF
    if (c <= 0xEF)
    {
        // Disallow overlong 3-byte sequence
        if ((c == 0xE0) && ((unsigned char)s[1] < 0xA0))
        {
            return 0;
        }

        // Disallow U+D800..U+DFFF
        if ((c == 0xED) && ((unsigned char)s[1] > 0x9F))
        {
            return 0;
        }

        // Make sure subsequent bytes are in the range 0x80..0xBF
        if (((unsigned char)s[1] & 0xC0) != 0x80)
        {
            return 0;
        }

        if (((unsigned char)s[2] & 0xC0) != 0x80)
        {
            return 0;
        }

        return 3;
    }

    // F5..FF
    if (c > 0xF4)
    {
        return 0;
    }

    // F0..F4: Disallow overlong 4-byte sequence
    if ((c == 0xF0) && ((unsigned char)s[1] < 0x90))
    {
        return 0;
    }

    // Disallow codepoints beyond U+10FFFF
    if ((c == 0xF4) && ((unsigned char)s[1] > 0x8F))
    {
        return 0;
    }

    // Make sure subsequent bytes are in the range 0x80..0xBF
    if (((unsigned char)s[1] & 0xC0) != 0x80)
    {
        return 0;
    }

    if (((unsigned char)s[2] & 0xC0) != 0x80)
    {
        return 0;
    }

    if (((unsigned char)s[3] & 0xC0) != 0x80)
    {
        return 0;
    }

    return 4;
}

/*********************************************************************//**
**
** Utf8ReadChar
**
** Reads the single UTF-8 character starting at the specified position in a string
**
** NOTE: This function assumes that the input is valid UTF-8, and that there are enough characters in front of s
**
** \param   s - pointer to the first byte of the character to read
** \param   out - pointer to variable in which to return the codepoint of the character
**
** \return  length of the character in bytes (1 thru 4)
**
**************************************************************************/
int Utf8ReadChar(const char *s, uchar_t *out)
{
    const unsigned char *c;

    c = (const unsigned char *) s;
    assert(Utf8ValidateChar(s) != 0);

    // 00..7F
    if (c[0] <= 0x7F)
    {
        *out = c[0];
        return 1;
    }

    // C2..DF (unless input is invalid)
    if (c[0] <= 0xDF)
    {
        *out = (((uchar_t)c[0] & 0x1F) << 6) |
                ((uchar_t)c[1] & 0x3F);
        return 2;
    }

    // E0..EF
    if (c[0] <= 0xEF)
    {
        *out = (((uchar_t)c[0] &  0xF) << 12) |
               (((uchar_t)c[1] & 0x3F) << 6)  |
                ((uchar_t)c[2] & 0x3F);
        return 3;
    }

    // F0..F4 (unless input is invalid)
    *out = (((uchar_t)c[0] &  0x7) << 18) |
           (((uchar_t)c[1] & 0x3F) << 12) |
           (((uchar_t)c[2] & 0x3F) << 6)  |
            ((uchar_t)c[3] & 0x3F);
    return 4;
}

/*********************************************************************//**
**
** Utf8WriteChar
**
** Writes a single UTF-8 character to the caller's buffer
**
** \param   unicode - codepoint of the character to write. This must be U+0000..U+10FFFF, but not U+D800..U+DFFF
** \param   out - pointer to buffer in which to return the UTF-8 encoded character
**                NOTE: This buffer must have space for MAX_UTF8_CHAR_LEN bytes
**
** \return  length of the character written in bytes (1 thru 4)
**
**************************************************************************/
int Utf8WriteChar(uchar_t unicode, char *out)
{
    unsigned char *o;

    o = (unsigned char *) out;
    assert((unicode <= 0x10FFFF) && ((unicode < 0xD800) || (unicode > 0xDFFF)));

    // U+0000..U+007F
    if (unicode <= 0x7F)
    {
        o[0] = unicode;
        return 1;
    }

    // U+0080..U+07FF
    if (unicode <= 0x7FF)
    {
        o[0] = 0xC0 | (unicode >> 6);
        o[1] = 0x80 | (unicode & 0x3F);
        return 2;
    }

    // U+0800..U+FFFF
    if (unicode <= 0xFFFF)
    {
        o[0] = 0xE0 | (unicode >> 12);
        o[1] = 0x80 | ((unicode >> 6) & 0x3F);
        o[2] = 0x80 | (unicode & 0x3F);
        return 3;
    }

    // U+10000..U+10FFFF
    o[0] = 0xF0 | (unicode >> 18);
    o[1] = 0x80 | ((unicode >> 12) & 0x3F);
    o[2] = 0x80 | ((unicode >> 6) & 0x3F);
    o[3] = 0x80 | (unicode & 0x3F);
    return 4;
}

/*********************************************************************//**
**
** FromSurrogatePair
**
** Computes the Unicode codepoint of a UTF-16 surrogate pair
**
** \param   uc - first code unit of the surrogate pair. This should be 0xD800..0xDBFF
** \param   lc - second code unit of the surrogate pair. This should be 0xDC00..0xDFFF
** \param   unicode - pointer to variable in which to return the codepoint
**
** \return  true if the surrogate pair was valid, false otherwise
**
**************************************************************************/
bool FromSurrogatePair(uint16_t uc, uint16_t lc, uchar_t *unicode)
{
    if ((uc < 0xD800) || (uc > 0xDBFF))
    {
        return false;
    }

    if ((lc < 0xDC00) || (lc > 0xDFFF))
    {
        return false;
    }

    *unicode = 0x10000 + ((((uchar_t)uc & 0x3FF) << 10) | (lc & 0x3FF));
    return true;
}

/*********************************************************************//**
**
** ToSurrogatePair
**
** Constructs a UTF-16 surrogate pair, given a Unicode codepoint
**
** \param   unicode - codepoint to convert. This must be U+10000..U+10FFFF
** \param   uc - pointer to variable in which to return the first code unit of the surrogate pair
** \param   lc - pointer to variable in which to return the second code unit of the surrogate pair
**
** \return  None
**
**************************************************************************/
void ToSurrogatePair(uchar_t unicode, uint16_t *uc, uint16_t *lc)
{
    uchar_t n;

    assert((unicode >= 0x10000) && (unicode <= 0x10FFFF));

    n = unicode - 0x10000;
    *uc = ((n >> 10) & 0x3FF) | 0xD800;
    *lc = (n & 0x3FF) | 0xDC00;
}

/*********************************************************************//**
**
** SbInit
**
** Initialises the specified string buffer, allocating an initial buffer for it
**
** \param   sb - pointer to string buffer to initialise
**
** \return  None
**
**************************************************************************/
void SbInit(SB *sb)
{
    memset(sb, 0, sizeof(SB));

    sb->start = (char *) malloc(17);
    if (sb->start == NULL)
    {
        OutOfMemory();
    }
    sb->cur = sb->start;
    sb->end = &sb->start[16];
}

/*********************************************************************//**
**
** SbNeed
**
** Ensures that the specified string buffer has space for the specified number of characters
**
** \param   sb - pointer to string buffer
** \param   need - number of characters that the caller wants to write to the buffer
**
** \return  None
**
**************************************************************************/
void SbNeed(SB *sb, int need)
{
    if ((sb->end - sb->cur) < need)
    {
        SbGrow(sb, need);
    }
}

/*********************************************************************//**
**
** SbGrow
**
** Grows the specified string buffer, until it has space for the specified number of characters
**
** \param   sb - pointer to string buffer
** \param   need - number of characters that the caller wants to write to the buffer
**
** \return  None
**
**************************************************************************/
void SbGrow(SB *sb, int need)
{
    size_t length;
    size_t alloc;

    length = (size_t)(sb->cur - sb->start);
    alloc = (size_t)(sb->end - sb->start);

    do
    {
        alloc *= 2;
    } while (alloc < length + (size_t)need);

    // NOTE: One extra character is allocated, to allow SbFinish() to NULL terminate the buffer
    sb->start = (char *) realloc(sb->start, alloc + 1);
    if (sb->start == NULL)
    {
        OutOfMemory();
    }
    sb->cur = &sb->start[length];
    sb->end = &sb->start[alloc];
}

/*********************************************************************//**
**
** SbPut
**
** Writes the specified number of characters to the specified string buffer
**
** \param   sb - pointer to string buffer
** \param   bytes - pointer to characters to write
** \param   count - number of characters to write
**
** \return  None
**
**************************************************************************/
void SbPut(SB *sb, const char *bytes, int count)
{
    SbNeed(sb, count);
    memcpy(sb->cur, bytes, count);
    sb->cur = &sb->cur[count];
}

/*********************************************************************//**
**
** SbPutChar
**
** Writes a single character to the specified string buffer
**
** \param   sb - pointer to string buffer
** \param   c - character to write
**
** \return  None
**
**************************************************************************/
void SbPutChar(SB *sb, char c)
{
    SbNeed(sb, 1);
    sb->cur[0] = c;
    sb->cur = &sb->cur[1];
}

/*********************************************************************//**
**
** SbPuts
**
** Writes the specified NULL terminated string to the specified string buffer
**
** \param   sb - pointer to string buffer
** \param   str - pointer to NULL terminated string to write
**
** \return  None
**
**************************************************************************/
void SbPuts(SB *sb, const char *str)
{
    SbPut(sb, str, strlen(str));
}

/*********************************************************************//**
**
** SbFinish
**
** NULL terminates the specified string buffer, and returns the string that it contains
**
** \param   sb - pointer to string buffer
**
** \return  pointer to the dynamically allocated NULL terminated string containing the contents of the buffer
**          NOTE: Ownership of the returned string passes to the caller
**
**************************************************************************/
char *SbFinish(SB *sb)
{
    sb->cur[0] = '\0';
    assert(sb->start <= sb->cur);
    assert(strlen(sb->start) == (size_t)(sb->cur - sb->start));

    return sb->start;
}

/*********************************************************************//**
**
** SbFree
**
** Frees the buffer owned by the specified string buffer
**
** \param   sb - pointer to string buffer
**
** \return  None
**
**************************************************************************/
void SbFree(SB *sb)
{
    free(sb->start);
}

/*********************************************************************//**
**
** StrDup
**
** Takes a copy of the specified string
** NOTE: This function exists because, sadly, strdup() is not portable
**
** \param   str - pointer to NULL terminated string to copy
**
** \return  pointer to dynamically allocated copy of the string
**          NOTE: Ownership of the returned string passes to the caller
**
**************************************************************************/
char *StrDup(const char *str)
{
    char *ret;

    ret = (char *) malloc(strlen(str) + 1);
    if (ret == NULL)
    {
        OutOfMemory();
    }
    strcpy(ret, str);

    return ret;
}

/*********************************************************************//**
**
** OutOfMemory
**
** Exits the executable, after printing that memory could not be allocated
**
** \param   None
**
** \return  None - this function does not return
**
**************************************************************************/
void OutOfMemory(void)
{
    fprintf(stderr, "Out of memory.\n");
    exit(EXIT_FAILURE);
}
