/**
 * Copyright (c) 2007-2012, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __data_parser_hh
#define __data_parser_hh

#include <stdio.h>

#include <openssl/sha.h>

#include <list>
#include <vector>
#include <iterator>
#include <algorithm>

#include "pcrepp.hh"
#include "byte_array.hh"
#include "data_scanner.hh"

/**
 * Switch to the 'parser' view mode when the user hits ';' so they
 * can easily see what columns are available.
 *
 * select * from logline;
 * select itemfrom(csv_key, 0) from logline;
 * select itemfrom(csv_key, -1) from logline;
 * select itemfrom(dict_key, "key") from logline;
 * select itemfrom(dict_key, "key[0]") from logline;
 * select itemfrom(csv_key, 0:3) from logline; support splices ?
 *
 * Add a command to create a logline table with a given name so the user can
 * do joins across the tables:
 *
 *   create-logline-table sudo_logline
 *   select * from logline, sudo_logline where sudo_logline.COMMAND=logline.COMMAND;
 *
 * select timestmap / 60 as minute, sc_status, count(*) from access_log
 *     group by minute, sc_status
 *     order by minute, sc_status desc;
 *  (use group_concat() here?)
 *
 * The "itemfrom()" function parses the group and lets you specify an
 * expression to query the contents.
 *
 * For 'report-on' command:
 *  'report-on PWD'
 *   select PWD,count(*) as amount from logline group by PWD order by amount desc;
 *  'report-on num_col num_col2'
 *   select avg(num_col),stddev(num_col),... from logline;
 * Instead of a command, we should automatically create views with the
 * relevant select statements.
 *
 * Add a tojson() aggregate function to sqlite:
 *   select foo,tojson(bar) group by foo;
 *
 *   1 ["a", "b", "c"]
 *   2 ["d", "e", "f"]
 *
 * We should automatically detect sqlite files provided on the command line
 * and attach the database.
 *
 * add a 'metadata' view that has all the metadata crud (sql tables/log)
 *
 * Add support for sqlite_log that writes to a temp file and is displayed in the
 * metadata view.
 *
 * Add a function that bookmarks all lines in the log view based on line_numbers
 * in the sql query result.
 *
 *    select line_number from logline where A="b";
 *    hit 'y/Y' to move forward and backwards through sql results
 *    hit 'R' key and all the lines are bookmarked
 *
 * add path manipulation functions like basename, dirname, splitext
 *
 * use the vt52_curses emulation to embed a editor for editing queries.  For
 * example, you could hit 'ctrl+;' and it would split the window in half with
 * the bottom being used for nano.  When the file was written, lnav should
 * notice and do a 'prepare' on the sql to make sure it is correct.
 *
 * Maybe add other tables for accessing lnav state.  For example, you could do
 * a query to find log lines of interest and then insert their line numbers
 * into the 'bookmarks' table to create new user bookmarks.
 */


template<class Container, class UnaryPredicate>
void strip(Container &container, UnaryPredicate p)
{
    while (!container.empty() && p(container.front())) {
        container.pop_front();
    }
    while (!container.empty() && p(container.back())) {
        container.pop_back();
    }
}

enum data_format_state_t {
    DFS_ERROR = -1,
    DFS_INIT,
    DFS_KEY,
    DFS_VALUE,
};

struct data_format {
    data_format(data_token_t appender = DT_INVALID,
                data_token_t terminator = DT_INVALID)
        : df_appender(appender), df_terminator(terminator)
    {};

    const data_token_t df_appender;
    const data_token_t df_terminator;
};

data_format_state_t dfs_semi_next(data_format_state_t state,
                                  data_token_t next_token);
data_format_state_t dfs_comma_next(data_format_state_t state,
                                   data_token_t next_token);

class data_parser {
public:
    static data_format FORMAT_SEMI;
    static data_format FORMAT_COMMA;
    static data_format FORMAT_PLAIN;

    typedef byte_array<20>     schema_id_t;

    struct element;
    typedef std::list<element> element_list_t;

    struct element {
        element() : e_token(DT_INVALID), e_sub_elements(NULL) { };
        element(element_list_t &subs,
                data_token_t token,
                bool assign_subs_elements = true)
            : e_capture(subs.front().e_capture.c_begin,
                        subs.back().e_capture.c_end),
              e_token(token),
              e_sub_elements(NULL)
        {
            if (assign_subs_elements) {
                this->assign_elements(subs);
            }
        };

        element(const element &other)
        {
            /* assert(other.e_sub_elements == NULL); */

            this->e_capture      = other.e_capture;
            this->e_token        = other.e_token;
            this->e_sub_elements = NULL;
            if (other.e_sub_elements != NULL) {
                this->assign_elements(*other.e_sub_elements);
            }
        };

        ~element()
        {
            if (this->e_sub_elements != NULL) {
                delete this->e_sub_elements;
                this->e_sub_elements = NULL;
            }
        };

        element & operator=(const element &other)
        {
            this->e_capture      = other.e_capture;
            this->e_token        = other.e_token;
            this->e_sub_elements = NULL;
            if (other.e_sub_elements != NULL) {
                this->assign_elements(*other.e_sub_elements);
            }
            return *this;
        };

        void                    assign_elements(element_list_t &subs)
        {
            if (this->e_sub_elements == NULL) {
                this->e_sub_elements = new element_list_t();
            }
            this->e_sub_elements->swap(subs);
            this->update_capture();
        };

        void                    update_capture(void)
        {
            if (this->e_sub_elements != NULL) {
                this->e_capture.c_begin =
                    this->e_sub_elements->front().e_capture.c_begin;
                this->e_capture.c_end =
                    this->e_sub_elements->back().e_capture.c_end;
            }
        };

        data_token_t            value_token(void) const
        {
            data_token_t retval = DT_INVALID;

            if (this->e_token == DNT_VALUE &&
                this->e_sub_elements != NULL &&
                this->e_sub_elements->size() == 1) {
                retval = this->e_sub_elements->front().e_token;
            }
            return retval;
        };

        void                    print(FILE *out, pcre_input &pi, int offset =
                                          0)
        {
            int lpc;

            if (this->e_sub_elements != NULL) {
                for (element_list_t::iterator iter2 =
                         this->e_sub_elements->begin();
                     iter2 != this->e_sub_elements->end();
                     ++iter2) {
                    iter2->print(out, pi, offset + 1);
                }
            }

            fprintf(out, "%4s %3d:%-3d ",
                    data_scanner::token2name(this->e_token),
                    this->e_capture.c_begin,
                    this->e_capture.c_end);
            for (lpc = 0; lpc < this->e_capture.c_end; lpc++) {
                if (lpc == this->e_capture.c_begin) {
                    fputc('^', out);
                }
                else if (lpc == (this->e_capture.c_end - 1)) {
                    fputc('^', out);
                }
                else if (lpc > this->e_capture.c_begin) {
                    fputc('-', out);
                }
                else{
                    fputc(' ', out);
                }
            }
            for (; lpc < (int)pi.pi_length; lpc++) {
                fputc(' ', out);
            }

            std::string sub = pi.get_substr(&this->e_capture);
            fprintf(out, "  %s\n", sub.c_str());
        };

        pcre_context::capture_t e_capture;
        data_token_t            e_token;

        element_list_t *        e_sub_elements;
    };

    struct element_cmp {
        bool operator()(data_token_t token, const element &elem) const
        {
            return token == elem.e_token || token == DT_ANY;
        };

        bool operator()(const element &elem, data_token_t token) const
        {
            return (*this)(token, elem);
        };
    };

    struct element_if {
        element_if(data_token_t token) : ei_token(token) { };

        bool operator()(const element &a) const
        {
            return a.e_token == this->ei_token;
        };

private:
        data_token_t ei_token;
    };

    data_parser(data_scanner *ds) : dp_format(NULL), dp_scanner(ds) { };

    void pairup(schema_id_t *schema, element_list_t &pairs_out,
                element_list_t &in_list)
    {
        element_list_t el_stack, free_row, key_comps, value, prefix;
        SHA_CTX        context;

        for (element_list_t::iterator iter = in_list.begin();
             iter != in_list.end();
             ++iter) {
            if (iter->e_token == DNT_GROUP) {
                element_list_t group_pairs;

                this->pairup(NULL, group_pairs, *iter->e_sub_elements);
                if (!group_pairs.empty()) {
                    iter->assign_elements(group_pairs);
                }
            }

            if (iter->e_token == DT_SEPARATOR) {
                element_list_t::iterator key_iter = key_comps.end();
                bool found = false;

                --key_iter;
                for (;
                     key_iter != key_comps.begin() && !found;
                     --key_iter) {
                    if (key_iter->e_token == this->dp_format->df_appender) {
                        ++key_iter;
                        value.splice(value.end(),
                                     key_comps,
                                     key_comps.begin(),
                                     key_iter);
                        key_comps.splice(key_comps.begin(),
                                         key_comps,
                                         key_comps.end());
                        key_comps.resize(1);
                        found = true;
                    }
                    else if (key_iter->e_token ==
                             this->dp_format->df_terminator) {
                        std::vector<element> key_copy;

                        value.splice(value.end(),
                                     key_comps,
                                     key_comps.begin(),
                                     key_iter);
                        ++key_iter;
                        key_comps.pop_front();
                        strip(key_comps, element_if(DT_WHITE));
                        found = true;
                    }
                }
                if (!found && !el_stack.empty() && !key_comps.empty()) {
                    element_list_t::iterator value_iter;

                    value.splice(value.end(),
                                 key_comps,
                                 key_comps.begin(),
                                 key_comps.end());
                    value_iter = value.end();
                    std::advance(value_iter, -1);
                    key_comps.splice(key_comps.begin(),
                                     value,
                                     value_iter);
                    key_comps.resize(1);
                }
                strip(value, element_if(DT_WHITE));
                value.remove_if(element_if(DT_COMMA));
                if (!value.empty()) {
                    el_stack.push_back(element(value, DNT_VALUE));
                }
                strip(key_comps, element_if(DT_WHITE));
                if (!key_comps.empty()) {
                    el_stack.push_back(element(key_comps, DNT_KEY, false));
                }
                key_comps.clear();
                value.clear();
            }
            else {
                key_comps.push_back(*iter);
            }
        }

        if (el_stack.empty()) {
            free_row.splice(free_row.begin(),
                            key_comps, key_comps.begin(), key_comps.end());
        }
        else {
            value.splice(value.begin(),
                         key_comps,
                         key_comps.begin(),
                         key_comps.end());
            strip(value, element_if(DT_WHITE));
            value.remove_if(element_if(DT_COMMA));
            if (!value.empty()) {
                el_stack.push_back(element(value, DNT_VALUE));
            }
        }

        SHA_Init(&context);
        while (!el_stack.empty()) {
            element_list_t::iterator kv_iter = el_stack.begin();
            if (kv_iter->e_token == DNT_VALUE) {
                free_row.push_back(el_stack.front());
            }
            if (kv_iter->e_token != DNT_KEY) {
                el_stack.pop_front();
                continue;
            }

            ++kv_iter;
            if (kv_iter == el_stack.end()) {
                el_stack.pop_front();
                continue;
            }

            if (kv_iter->e_token != DNT_VALUE) {
                el_stack.pop_front();
                continue;
            }

            std::string key_val =
                this->get_element_string(el_stack.front());
            element_list_t pair_subs;

            if (schema != NULL) {
                SHA_Update(&context, key_val.c_str(), key_val.length());
            }

            ++kv_iter;
            pair_subs.splice(pair_subs.begin(),
                             el_stack,
                             el_stack.begin(),
                             kv_iter);
            pairs_out.push_back(element(pair_subs, DNT_PAIR));
        }

        if (pairs_out.size() == 1) {
            element &pair  = pairs_out.front();
            element &value = pair.e_sub_elements->back();

            if (value.e_token == DNT_VALUE &&
                value.e_sub_elements != NULL &&
                value.e_sub_elements->size() > 1) {
                prefix.splice(prefix.begin(),
                              *pair.e_sub_elements,
                              pair.e_sub_elements->begin());
                free_row.clear();
                free_row.splice(free_row.begin(),
                                *value.e_sub_elements,
                                value.e_sub_elements->begin(),
                                value.e_sub_elements->end());
                pairs_out.clear();
                SHA_Init(&context);
            }
        }

        if (pairs_out.empty() && !free_row.empty()) {
            while (!free_row.empty()) {
                switch (free_row.front().e_token) {
                case DNT_GROUP:
                case DT_NUMBER:
                case DT_SYMBOL:
                case DT_HEX_NUMBER:
                case DT_OCTAL_NUMBER:
                case DT_VERSION_NUMBER:
                case DT_QUOTED_STRING:
                case DT_IPV4_ADDRESS:
                case DT_IPV6_ADDRESS:
                case DT_MAC_ADDRESS:
                case DT_UUID:
                case DT_URL:
                case DT_PATH:
                case DT_TIME:
                case DT_PERCENTAGE: {
                    element_list_t pair_subs;
                    struct element blank;

                    blank.e_capture.c_begin = blank.e_capture.c_end =
                                                  free_row.front().e_capture.
                                                  c_begin;
                    blank.e_token = DNT_KEY;
                    pair_subs.push_back(blank);
                    pair_subs.push_back(free_row.front());
                    pairs_out.push_back(element(pair_subs, DNT_PAIR));
                }
                break;

                default: {
                    std::string key_val = this->get_element_string(
                        free_row.front());

                    SHA_Update(&context, key_val.c_str(), key_val.length());
                }
                break;
                }

                free_row.pop_front();
            }
        }

        if (!prefix.empty()) {
            element_list_t pair_subs;
            struct element blank;

            blank.e_capture.c_begin = blank.e_capture.c_end =
                                          prefix.front().e_capture.c_begin;
            blank.e_token = DNT_KEY;
            pair_subs.push_back(blank);
            pair_subs.push_back(prefix.front());
            pairs_out.push_front(element(pair_subs, DNT_PAIR));
        }

        if (schema != NULL) {
            SHA_Final(this->dp_schema_id.ba_data, &context);
        }
    };

    void discover_format(void)
    {
        pcre_context_static<30> pc;
        int            hist[DT_TERMINAL_MAX];
        struct element elem;

        this->dp_group_token.push_back(DT_INVALID);
        this->dp_group_stack.resize(1);

        data_format_state_t semi_state  = DFS_INIT;
        data_format_state_t comma_state = DFS_INIT;

        memset(hist, 0, sizeof(hist));
        while (this->dp_scanner->tokenize(pc, elem.e_token)) {
            pcre_context::iterator pc_iter;

            pc_iter = std::find_if(pc.begin(), pc.end(), capture_if_not(-1));
            assert(pc_iter != pc.end());

            elem.e_capture = *pc_iter;

            assert(elem.e_capture.c_begin != -1);
            assert(elem.e_capture.c_end != -1);

            semi_state          = dfs_semi_next(semi_state, elem.e_token);
            comma_state         = dfs_comma_next(comma_state, elem.e_token);
            hist[elem.e_token] += 1;
            switch (elem.e_token) {
            case DT_LPAREN:
            case DT_LANGLE:
            case DT_LCURLY:
            case DT_LSQUARE:
                this->dp_group_token.push_back(elem.e_token);
                this->dp_group_stack.push_back(element_list_t());
                break;

            case DT_RPAREN:
            case DT_RANGLE:
            case DT_RCURLY:
            case DT_RSQUARE:
                if (this->dp_group_token.back() == (elem.e_token - 1)) {
                    this->dp_group_token.pop_back();

                    std::list<element_list_t>::reverse_iterator riter =
                        this->dp_group_stack.rbegin();
                    ++riter;
                    if (!this->dp_group_stack.back().empty()) {
                        (*riter).push_back(element(this->dp_group_stack.back(),
                                                   DNT_GROUP));
                    }
                    this->dp_group_stack.pop_back();
                }
                else {
                    this->dp_group_stack.back().push_back(elem);
                }
                break;

            default:
                this->dp_group_stack.back().push_back(elem);
                break;
            }
        }

        while (this->dp_group_stack.size() > 1) {
            this->dp_group_token.pop_back();

            std::list<element_list_t>::reverse_iterator riter =
                this->dp_group_stack.rbegin();
            ++riter;
            if (!this->dp_group_stack.back().empty()) {
                (*riter).push_back(element(this->dp_group_stack.back(),
                                           DNT_GROUP));
            }
            this->dp_group_stack.pop_back();
        }

        if (semi_state != DFS_ERROR && hist[DT_SEMI]) {
            this->dp_format = &FORMAT_SEMI;
        }
        else if (comma_state != DFS_ERROR) {
            this->dp_format = &FORMAT_COMMA;
        }
        else {
            this->dp_format = &FORMAT_PLAIN;
        }
    };

    void parse(void)
    {
        this->discover_format();

        this->pairup(&this->dp_schema_id,
                     this->dp_pairs,
                     this->dp_group_stack.front());

        for (element_list_t::iterator iter = this->dp_pairs.begin();
             iter != this->dp_pairs.end();
             ++iter) {
            if (iter->e_token == DNT_PAIR) {
                element_list_t &pair_subs = *iter->e_sub_elements;
                std::string     key_val   = this->get_element_string(
                    pair_subs.front());
            }
        }
    };

    std::string get_element_string(element &elem)
    {
        pcre_input &pi = this->dp_scanner->get_input();

        return pi.get_substr(&elem.e_capture);
    };

    void print(FILE *out, element_list_t &el)
    {
        fprintf(out, "             %s\n",
                this->dp_scanner->get_input().get_string());
        for (element_list_t::iterator iter = el.begin();
             iter != el.end();
             ++iter) {
            iter->print(out, this->dp_scanner->get_input());
        }
    };

    std::vector<data_token_t> dp_group_token;
    std::list<element_list_t> dp_group_stack;

    element_list_t dp_errors;

    element_list_t dp_pairs;
    schema_id_t    dp_schema_id;
    data_format *  dp_format;

private:
    data_scanner *dp_scanner;
};
#endif
